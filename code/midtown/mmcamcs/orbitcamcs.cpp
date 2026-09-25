/*
    Open1560 - An Open Source Re-Implementation of Midtown Madness 1 Beta
    Copyright (C) 2020 Brick

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

define_dummy_symbol(mmcamcs_orbitcamcs);

#include "orbitcamcs.h"

#include "arts7/sim.h"
#include "eventq7/event.h"
#include "mmcar/car.h"
#include "mmcar/trailer.h"
#include "mmdyna/isect.h"
#include "mmdyna/poly.h"
#include "mmphysics/phys.h"

static mem::cmd_param PARAM_orbitsens {"orbitsens", "Orbital camera mouse sensitivity, in radians per mouse count"};
static mem::cmd_param PARAM_orbitdist {"orbitdist", "Orbital camera distance from the car"};
static mem::cmd_param PARAM_orbitheight {"orbitheight", "Orbital camera height above the car's origin"};
static mem::cmd_param PARAM_orbitinvertx {"orbitinvertx", "Invert the orbital camera's horizontal axis"};
static mem::cmd_param PARAM_orbitinverty {"orbitinverty", "Invert the orbital camera's vertical axis"};
static mem::cmd_param PARAM_orbitsmooth {"orbitsmooth", "Orbital camera smoothing rate; higher is snappier, 0 is off"};
static mem::cmd_param PARAM_orbitrecenter {
    "orbitrecenter", "Seconds of mouse idle before the orbital camera eases back behind the car; 0 is off"};
static mem::cmd_param PARAM_orbitdrift {
    "orbitdrift", "How far the orbital camera follows the direction of travel in a slide; 0 locks it to the tail"};

static mem::cmd_param PARAM_orbitdistmin {"orbitdistmin", "Speed the camera starts pulling back at, in mph"};
static mem::cmd_param PARAM_orbitdistmax {"orbitdistmax", "Speed the camera is fully pulled back at, in mph"};
static mem::cmd_param PARAM_orbitpullback {"orbitpullback", "Metres the camera eases back by at full speed"};
static mem::cmd_param PARAM_orbitpulldown {"orbitpulldown", "Metres the camera drops by at full speed"};
static mem::cmd_param PARAM_orbitfov {"orbitfov", "Degrees of FOV widening at full speed; 0 is off"};
static mem::cmd_param PARAM_orbitcollide {
    "orbitcollide", "Keep the orbital camera out of world geometry; 0 lets it pass through"};

// Kept clear of +/- PI/2 so the view never degenerates looking straight down or up.
static constexpr f32 OrbitPitchMin = -0.35f;
static constexpr f32 OrbitPitchMax = 1.35f;

// Resting pitch. Low: most of the downward angle already comes from Height being above the point
// the camera orbits, so a large value here on top of that ends up staring down at the roof.
static constexpr f32 OrbitPitchStart = 0.08f;

// How fast the recentre eases in. Slower than the input filter, so it reads as the camera settling
// back rather than being yanked, but quick enough that it feels tied to the car.
static constexpr f32 OrbitRecenterRate = 3.5f;

// Ground speed, in m/s, below which the direction of travel is too noisy to steer the camera by.
static constexpr f32 OrbitDriftMinSpeed = 4.0f;

// Furthest the camera will swing towards the direction of travel, in radians. Enough to put the
// side of the car on screen in a slide without ever losing the car's tail.
static constexpr f32 OrbitDriftMaxSlip = 0.70f;

// Past this slip angle the car is reversing or spinning rather than drifting, and following the
// direction of travel would whip the camera round the front. Fall back to the way it is pointing.
static constexpr f32 OrbitDriftIgnore = 2.0f;

// Below this speed the car is parked or crawling, and pulling the camera round behind it fights the
// player rather than helping. In mph, like the speed bands.
static constexpr f32 OrbitRecenterMinSpeed = 6.0f;

// Largest mouse movement honoured in a single frame. Bounds a flick that arrives as one enormous
// delta - the mouse leaving the window, or a stretch of frames where input was suppressed.
static constexpr i32 OrbitMaxMouseStep = 250;

// --- Collision -------------------------------------------------------------------------------

// How much clear space is kept between the eye and whatever it was stopped by, in metres. Large
// enough that the near plane (CameraNear, 0.5 below) does not slice into the surface, and that the
// shake applied after the solve cannot cross it.
static constexpr f32 OrbitCollideSkin = 0.6f;

// Closest the camera will ever be pulled, in metres. Below this the car fills the frame and the view
// is useless anyway, so an obstruction this severe is better answered by giving up and letting the
// geometry pass than by shoving the eye into the driver's seat.
static constexpr f32 OrbitCollideMin = 1.2f;

// Vertical clearance kept above whatever is directly under the eye, in metres, and the half-length
// of the probe that measures it.
static constexpr f32 OrbitCollideGround = 0.75f;

// Metres per second the solve is allowed to move the camera by.
//
// The asymmetry is the whole trick. Pulling IN is near-instant, because a frame spent inside a wall
// is a frame looking at the inside of the world and there is no graceful way to ease into that.
// Pushing back OUT is slow, because the obstruction clearing is not something the player asked for,
// and a camera that springs back the moment it can is far more distracting than one that takes half
// a second to notice.
static constexpr f32 OrbitCollidePullIn = 60.0f;
static constexpr f32 OrbitCollidePushOut = 3.0f;

// Peak rotational displacement at full amplitude, in radians. Roll is allowed the most: rolling the
// view reads as violence without ever pointing the camera away from the car, which is why it
// carries most of the effect here. All three are small - shake that is obvious on a screenshot is
// far too strong in motion.
static constexpr f32 OrbitShakePitch = 0.016f;
static constexpr f32 OrbitShakeYaw = 0.013f;
static constexpr f32 OrbitShakeRoll = 0.024f;

// Peak positional displacement, in metres, along the camera's own right and up axes.
static constexpr f32 OrbitShakeOffset = 0.05f;

// Envelope rates. Attack is quicker than release everywhere: effects should arrive promptly and
// leave gently, or the camera feels twitchy.
static constexpr f32 OrbitSpeedEnvelopeRate = 2.5f;

// Reduces an angle to [-PI, PI] so yaw never winds up and differences take the short way round.
static f32 OrbitWrapAngle(f32 angle)
{
    return std::remainder(angle, 2.0f * ARTS_PI);
}

// Frame-rate independent exponential approach: the same fraction of the remaining distance is
// covered per second regardless of how the frames fall.
static f32 OrbitBlend(f32 rate, f32 delta)
{
    return 1.0f - std::exp(-rate * delta);
}

void OrbitCamCS::Init(mmCar* car, mmViewCS* view)
{
    Car = car;
    View = view;
    CarMatrix = &Car->Sim.LCS.World;
    SetName(Car->Sim.GetNodeName());

    // Sits in tight and low on the car at a standstill and opens out with speed, so the framing
    // itself carries the sense of pace rather than the shake having to do all of it. Height is
    // what sets most of the downward angle at these distances, so it stays modest.
    Distance = PARAM_orbitdist.get_or(6.0f);
    Height = PARAM_orbitheight.get_or(1.15f);
    SmoothRate = PARAM_orbitsmooth.get_or(14.0f);
    RecenterDelay = PARAM_orbitrecenter.get_or(0.6f);
    DriftBias = PARAM_orbitdrift.get_or(0.85f);

    f32 sensitivity = PARAM_orbitsens.get_or(0.0045f);

    YawScale = PARAM_orbitinvertx.get_or(false) ? sensitivity : -sensitivity;
    PitchScale = PARAM_orbitinverty.get_or(false) ? sensitivity : -sensitivity;

    // In mph. The framing band runs from a crawl to beyond what anything but a Panoz GTR-1 will
    // reach, so the fast cars keep opening out after a 140 mph car has run out of road.
    FrameStartSpeed = PARAM_orbitdistmin.get_or(5.0f);
    FrameMaxSpeed = PARAM_orbitdistmax.get_or(206.0f);

    SpeedDistance = PARAM_orbitpullback.get_or(6.0f);
    SpeedHeight = PARAM_orbitpulldown.get_or(-0.25f);

    // OFF by default, which is what the note at the widening itself always claimed and what the
    // 18.0f here contradicted.
    //
    // A camera that widens with speed is the one framing trick on this camera that RTX Remix cannot
    // follow. Remix reconstructs the camera from the projection matrix and has no previous-frame
    // projection to reproject through when the FOV moves - which is why it warns about it at all
    // ("CameraManager: FOV of a camera changed between frames"). Every other thing this camera does
    // to the framing is a change of POSITION, and a position change is exactly what the view matrix
    // already tells Remix; the pullback and drop below are free for that reason and stay on.
    //
    // Reading the closed camera assembly shows how unusual this is in the first place. The stock
    // cameras never touch the FOV in flight: TrackCamCS's constructor writes CameraFOV (BaseCamCS
    // +0x90) once, as 60 degrees, and nothing writes it again, while asCamera::SetView is only
    // reached from mmViewCS::SetCamera/Reset/NewCam and from UpdateView - camera changes, never a
    // frame update. mmViewCS::Update only copies the camera MATRIX out. So the game's projection is
    // constant for a whole race, and this was the one thing in the renderer that would have made it
    // move every frame.
    SpeedFov = PARAM_orbitfov.get_or(0.0f);

    CollideEnabled = PARAM_orbitcollide.get_or(true);

    Shake.Init();

    // BaseCamCS defaults this to 3.0, which clips the car when orbiting in close.
    CameraNear = 0.5f;

    BaseFov = CameraFOV;
}

void OrbitCamCS::MakeActive()
{
    if (!Car)
        return;

    // The interior cameras deactivate the car model, so make sure it is drawn again.
    Car->Model.Activate();

    if (Car->Trailer)
        Car->Trailer->Inst.SetFlags(INST_FLAG_ACTIVE);

    // Yawing to the car's own heading is what puts the camera behind it looking forward; adding
    // half a turn on top swings it round to the nose. Verified in game - the geometry of
    // PolarView's offset-then-rotate is easy to reason the wrong way round.
    if (CarMatrix)
        TargetYaw = OrbitWrapAngle(std::atan2(CarMatrix->m2.x, CarMatrix->m2.z));

    TargetPitch = OrbitPitchStart;

    // Snap rather than smooth into place: this runs as the camera is installed, and the transition
    // mmViewCS runs on top is already doing the blending from the previous camera.
    Yaw = TargetYaw;
    Pitch = TargetPitch;
    IdleTime = 0.0f;

    // Start from a clean slate so a crash from the previous stint does not shake the new one, and
    // so the first frame's derivatives are not taken against stale state.
    Shake.Reset();

    SpeedFactor = 0.0f;
    CollideDistance = 0.0f;
    HasCollideHistory = false;

    SyncMouse();
}

void OrbitCamCS::Update()
{
    f32 delta = Sim()->GetUpdateDelta();

    // Always re-read the accumulator, even when the input is being thrown away, so that resuming
    // never applies everything that happened while the camera was not listening.
    i32 mouse_x = PrevMouseX;
    i32 mouse_y = PrevMouseY;

    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        mouse_x = queue->GetMouseVirtualX();
        mouse_y = queue->GetMouseVirtualY();

        // Unbounded mouse travel while this camera is in use, so it can go all the way round the car in
        // a window too. Asked for every frame; the handler lets go once the requests stop. Not while
        // paused - the mouse belongs to the menu then.
        if (!Sim()->IsPaused())
            queue->BeginTracking();
    }

    i32 step_x = std::clamp(mouse_x - PrevMouseX, -OrbitMaxMouseStep, OrbitMaxMouseStep);
    i32 step_y = std::clamp(mouse_y - PrevMouseY, -OrbitMaxMouseStep, OrbitMaxMouseStep);

    PrevMouseX = mouse_x;
    PrevMouseY = mouse_y;

    // While the pause menu is up the mouse belongs to the menu, so the camera holds still. Nothing
    // below runs, which also freezes the shake rather than leaving it idling on a paused screen.
    if (!Sim()->IsPaused() && (delta > 0.0f))
    {
        // How fast the player is swinging the view, in the same units as the car's own yaw rate, so
        // the turn gate inside the shake can compare the two directly.
        f32 mouse_turn_rate = std::fabs(static_cast<f32>(step_x) * YawScale) / delta;

        if ((step_x != 0) || (step_y != 0))
        {
            TargetYaw = OrbitWrapAngle(TargetYaw + (static_cast<f32>(step_x) * YawScale));
            TargetPitch =
                std::clamp(TargetPitch + (static_cast<f32>(step_y) * PitchScale), OrbitPitchMin, OrbitPitchMax);

            IdleTime = 0.0f;
        }
        else
        {
            IdleTime += delta;

            Recenter(delta);
        }

        if (SmoothRate > 0.0f)
        {
            f32 blend = OrbitBlend(SmoothRate, delta);

            Yaw = OrbitWrapAngle(Yaw + (OrbitWrapAngle(TargetYaw - Yaw) * blend));
            Pitch += (TargetPitch - Pitch) * blend;
        }
        else
        {
            Yaw = TargetYaw;
            Pitch = TargetPitch;
        }

        UpdateFraming(delta);

        Shake.Update(Car, CarMatrix, delta, mouse_turn_rate);
    }

    f32 pitch = Pitch;
    f32 yaw = Yaw;
    f32 roll = 0.0f;

    const f32 shake = Shake.Strength();

    if (shake > 0.0f)
    {
        pitch += Shake.PitchNoise() * shake * OrbitShakePitch;
        yaw += Shake.YawNoise() * shake * OrbitShakeYaw;
        roll = Shake.RollNoise() * shake * OrbitShakeRoll;
    }

    // The point the camera orbits: the car's origin lifted to eye height. Everything below places
    // the eye relative to it, and it is also where the occlusion probe is fired FROM - a camera is
    // only obstructed if something stands between it and what it is looking at.
    Vector3 pivot {0.0f, Height + (SpeedHeight * SpeedFactor), 0.0f};

    if (CarMatrix)
        pivot += CarMatrix->m3;

    const f32 wanted = Distance + (SpeedDistance * SpeedFactor);

    camera_.PolarView(SolveCollision(wanted, pivot, yaw, std::clamp(pitch, OrbitPitchMin, OrbitPitchMax), roll), yaw,
        std::clamp(pitch, OrbitPitchMin, OrbitPitchMax), roll);

    camera_.m3 += pivot;

    if (shake > 0.0f)
    {
        // Along the camera's own right and up axes rather than world axes, so the jitter stays
        // perpendicular to the view and never pushes the camera into or away from the car.
        //
        // Applied AFTER the collision solve rather than before it, deliberately. These offsets are
        // centimetres and the solve leaves a margin far larger than that (OrbitCollideSkin), so the
        // shake cannot push the eye through a wall; feeding them into the probe instead would make
        // the pulled-in distance jitter with the noise, which is a far more visible fault than the
        // millimetre of margin it would buy.
        camera_.m3 += camera_.m0 * (Shake.RightNoise() * shake * OrbitShakeOffset);
        camera_.m3 += camera_.m1 * (Shake.UpNoise() * shake * OrbitShakeOffset);
    }

    // Ground clearance, in world Y rather than along the view. Pulling the camera in would raise it
    // too, but only in proportion to how far the view is pitched down, so it does nothing at all in
    // the level shot where the eye is most likely to be skimming a road surface. Lifting directly
    // always works and changes the framing less.
    camera_.m3.y += CollideLift;

    // Off by default - see the note at SpeedFov in Init() for why, and why the pullback and drop
    // above are not. CameraFOV only reaches the asCamera through UpdateView(), which nothing in the
    // original game calls per frame, so this has to push it through itself.
    if (SpeedFov != 0.0f)
    {
        f32 wanted_fov = BaseFov + (SpeedFov * SpeedFactor);

        if (std::fabs(wanted_fov - CameraFOV) > 0.05f)
        {
            CameraFOV = wanted_fov;

            UpdateView();
        }
    }
}

void OrbitCamCS::UpdateFraming(f32 delta)
{
    if (!Car)
        return;

    // Framing follows road speed, and reads mph directly so the tuning numbers line up with the
    // cars' quoted top speeds rather than having to be converted. The shake, by contrast, comes off
    // the engine - see mmCamShake, which owns all of that now.
    f32 frame_span = FrameMaxSpeed - FrameStartSpeed;
    f32 frame_target =
        (frame_span > 0.0f) ? std::clamp((Car->Sim.SpeedMPH - FrameStartSpeed) / frame_span, 0.0f, 1.0f) : 0.0f;

    SpeedFactor += (frame_target - SpeedFactor) * OrbitBlend(OrbitSpeedEnvelopeRate, delta);
}

// Where along the segment from `from` to `to` the world first gets in the way, as a fraction, or 1
// when it does not.
//
// PHYS_COLLIDE_ROOM is the static world - terrain, roads, buildings, kerbs - which is exactly the
// set a camera should be stopped by and excludes the movers it should not be (the player's own car
// most of all, since every probe here starts inside it).
//
// The hit DISTANCE is recovered from the polygon's own plane rather than read back off the
// intersection. mmIntersection::Position is never read anywhere else in this codebase, so its
// meaning after a segment test is not established; mmPolygon::PlaneN/PlaneD are public, are what
// mmPolygon::GetPlaneY itself works from, and give the exact crossing for the price of a dot
// product. A hit with no polygon behind it - possible in principle - is treated as a hit at the
// start of the segment, which is the conservative reading.
static f32 OrbitProbe(const Vector3& from, const Vector3& to)
{
    const Vector3 direction = to - from;

    if (direction.Mag2() < 1.0e-6f)
        return 1.0f;

    mmIntersection isect;
    isect.InitSegment(from, to, nullptr, 2, 0);

    if (!PHYS.Collide(&isect, PHYS_COLLIDE_ROOM))
        return 1.0f;

    const mmPolygon* poly = isect.HitPoly;

    if (!poly)
        return 0.0f;

    const f32 denominator = poly->PlaneN ^ direction;

    // Parallel to the plane. It cannot be crossed along this segment, whatever the broad-phase said.
    if (std::fabs(denominator) < 1.0e-6f)
        return 1.0f;

    return std::clamp(-((poly->PlaneN ^ from) + poly->PlaneD) / denominator, 0.0f, 1.0f);
}

// How far back the camera may actually sit this frame, given what is behind it.
//
// The camera is a point on a sphere around the car, so an obstruction is anything standing between
// the two - which makes this one segment test from the pivot out to where the eye wants to be. The
// eye then sits just short of whatever it hits, and the roads, walls and hillsides the view used to
// sink through simply push it in instead.
//
// The two rates are deliberately very different. Pulling IN is instant: a frame spent inside a wall
// is a frame looking at the inside of the world, and there is no such thing as easing into that
// gracefully. Pushing back OUT is slow, because the obstruction clearing is not something the player
// asked for and a camera that springs back the instant it can is far more distracting than one that
// takes half a second. That asymmetry is the whole trick, and it is why this keeps state at all
// rather than just returning the probe's answer.
f32 OrbitCamCS::SolveCollision(f32 wanted, const Vector3& pivot, f32 yaw, f32 pitch, f32 roll)
{
    if (!CollideEnabled || (wanted <= 0.0f))
        return wanted;

    // Where the eye would go with nothing in the way. Built with the same PolarView the draw below
    // uses, so the probe is aimed at the position actually being solved for rather than at an
    // approximation of it.
    Matrix34 wanted_view;
    wanted_view.PolarView(wanted, yaw, pitch, roll);

    const Vector3 wanted_eye = wanted_view.m3 + pivot;

    // Probe slightly past the eye, so the margin below is carved out of real clearance rather than
    // out of the distance to a surface the eye had already reached.
    const Vector3 probe_end = pivot + ((wanted_eye - pivot) * ((wanted + OrbitCollideSkin) / wanted));

    f32 allowed = wanted;

    const f32 hit = OrbitProbe(pivot, probe_end);

    if (hit < 1.0f)
        allowed = std::max((hit * (wanted + OrbitCollideSkin)) - OrbitCollideSkin, OrbitCollideMin);

    // A separate downward probe, because the segment above only finds what is between the car and
    // the camera and says nothing about what is directly UNDER it. Cresting a hill puts the eye over
    // ground the pivot has clear line of sight to, and without this it sinks into the road surface -
    // which is the "shows the car from underneath" case, and the one an occlusion test alone can
    // never catch.
    Matrix34 allowed_view;
    allowed_view.PolarView(allowed, yaw, pitch, roll);

    const Vector3 eye = allowed_view.m3 + pivot;

    // A segment centred on the eye, so it finds ground the eye has already sunk below as readily as
    // ground it is merely close to. The hit fraction maps back to a height directly: t = 0.5 is the
    // eye's own level, so the clearance is (2t - 1) * OrbitCollideGround, negative when underground.
    const Vector3 above = eye + Vector3 {0.0f, OrbitCollideGround, 0.0f};
    const Vector3 below = eye - Vector3 {0.0f, OrbitCollideGround, 0.0f};

    f32 lift = 0.0f;

    if (const f32 ground = OrbitProbe(above, below); ground < 1.0f)
    {
        const f32 clearance = ((2.0f * ground) - 1.0f) * OrbitCollideGround;

        lift = std::max(OrbitCollideGround - clearance, 0.0f);
    }

    // Rate limiting, in metres per second, applied to the answer rather than to the probe.
    const f32 delta = Sim()->GetUpdateDelta();

    if (!HasCollideHistory)
    {
        CollideDistance = allowed;
        CollideLift = lift;
        HasCollideHistory = true;

        return CollideDistance;
    }

    CollideDistance = (allowed < CollideDistance) ? std::max(allowed, CollideDistance - (OrbitCollidePullIn * delta))
                                                  : std::min(allowed, CollideDistance + (OrbitCollidePushOut * delta));

    // The lift is rate limited the same way and for the same reasons, but on its own accumulator:
    // it is a different correction with a different cause, and running them through one number would
    // let a wall clearing release the camera into the road.
    CollideLift = (lift > CollideLift) ? std::min(lift, CollideLift + (OrbitCollidePullIn * delta))
                                       : std::max(lift, CollideLift - (OrbitCollidePushOut * delta));

    return CollideDistance;
}

void OrbitCamCS::Recenter(f32 delta)
{
    if ((RecenterDelay <= 0.0f) || (IdleTime < RecenterDelay) || !Car || !CarMatrix)
        return;

    // Leave a parked car's camera where the player put it.
    if (Car->Sim.SpeedMPH < OrbitRecenterMinSpeed)
        return;

    f32 heading = std::atan2(CarMatrix->m2.x, CarMatrix->m2.z);
    f32 desired = heading;

    // Settle behind where the car is *going* rather than where it is pointing. Sliding separates
    // the two, which swings the camera round far enough to put the car's side on screen; coming
    // straight again closes the gap on its own. Nothing here has to detect a drift as an event -
    // the slip angle already is one.
    const Vector3& velocity = Car->Sim.ICS.LinearVelocity;
    f32 ground_speed2 = (velocity.x * velocity.x) + (velocity.z * velocity.z);

    if (ground_speed2 > (OrbitDriftMinSpeed * OrbitDriftMinSpeed))
    {
        f32 slip = OrbitWrapAngle(std::atan2(velocity.x, velocity.z) - heading);

        if (std::fabs(slip) < OrbitDriftIgnore)
            desired = heading + (std::clamp(slip, -OrbitDriftMaxSlip, OrbitDriftMaxSlip) * DriftBias);
    }

    f32 blend = OrbitBlend(OrbitRecenterRate, delta);

    TargetYaw = OrbitWrapAngle(TargetYaw + (OrbitWrapAngle(desired - TargetYaw) * blend));
    TargetPitch += (OrbitPitchStart - TargetPitch) * blend;
}

void OrbitCamCS::SyncMouse()
{
    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        PrevMouseX = queue->GetMouseVirtualX();
        PrevMouseY = queue->GetMouseVirtualY();
    }
}
