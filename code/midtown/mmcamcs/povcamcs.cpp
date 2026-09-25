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

define_dummy_symbol(mmcamcs_povcamcs);

#include "povcamcs.h"

#include "arts7/sim.h"
#include "camshake.h"
#include "eventq7/event.h"
#include "mmcar/car.h"

static mem::cmd_param PARAM_povbob {"povbob", "In-car head bob strength; 0 is off"};
static mem::cmd_param PARAM_povbobsway {"povbobsway", "How much the in-car head bob rotates the view"};
static mem::cmd_param PARAM_povlook {"povlook", "Mouse look from the in-car cameras; 0 locks the view forward"};
static mem::cmd_param PARAM_povlooksens {"povlooksens", "In-car look sensitivity, in radians per mouse count"};
static mem::cmd_param PARAM_povlooksmooth {"povlooksmooth", "In-car look smoothing rate; 0 is off"};
static mem::cmd_param PARAM_povlookrecenter {
    "povlookrecenter", "Seconds of mouse idle before the in-car view eases back to centre; 0 is off"};
static mem::cmd_param PARAM_povlookinvertx {"povlookinvertx", "Invert the in-car look's horizontal axis"};
static mem::cmd_param PARAM_povlookinverty {"povlookinverty", "Invert the in-car look's vertical axis"};

// Peak rotational displacement of the bob at full amplitude, in radians.
//
// Smaller than the orbital camera's, and weighted differently. From the driver's seat the view IS
// the head, so rotating it reads as being shaken by the neck and goes wrong very quickly, while the
// same motion applied to the eye position reads as the whole body moving with the car. Roll is
// allowed the most of the three because it is the one a real head genuinely does.
static constexpr f32 PovBobPitch = 0.010f;
static constexpr f32 PovBobYaw = 0.008f;
static constexpr f32 PovBobRoll = 0.018f;

// Peak positional displacement, in metres. Larger than the orbital camera's, which is the whole
// difference between a camera vibrating and a head bobbing.
static constexpr f32 PovBobRight = 0.030f;
static constexpr f32 PovBobUp = 0.045f;
static constexpr f32 PovBobForward = 0.025f;

// How far the head is thrown fore and aft by acceleration alone, in metres, on top of the noise.
//
// This is the part that makes it read as a person rather than a camera mount. The noise channels
// vibrate about a fixed point; this leans, and it leans with the signed acceleration the shake model
// already measures, so hard throttle pushes the head back into the seat and braking pitches it
// towards the wheel. Deliberately not fed through the shake's strength - a lean is not a vibration,
// and it belongs there whenever the car is actually accelerating.
static constexpr f32 PovLeanAccel = 0.055f;

// How far the head may turn, in radians. Yaw reaches a little past the side window without letting
// the player look through their own seat; pitch covers the instruments to the top of the windscreen.
static constexpr f32 PovLookYawLimit = 2.0f;
static constexpr f32 PovLookPitchMin = -0.9f;
static constexpr f32 PovLookPitchMax = 0.7f;

// How fast the view eases back to straight ahead once the mouse goes quiet. Slower than the input
// filter, so it reads as the head settling rather than snapping forward.
static constexpr f32 PovRecenterRate = 3.0f;

// Largest mouse movement honoured in a single frame - see OrbitCamCS, same reasoning.
static constexpr i32 PovMaxMouseStep = 250;

// The state, at file scope rather than on the class, because PovCamCS's layout is fixed by
// check_size(PovCamCS, 0x144) - it is a class the assembly indexes, so it cannot grow a member.
//
// One instance shared by both in-car cameras is not a compromise here, it is the better behaviour:
// mmViewCS updates whichever camera is current, only one view is ever current, and sharing the phase
// means switching between the dash and the bumper does not restart the vibration or drop the lean.
struct PovHeadState
{
    mmCamShake Shake {};

    // Where the view is pointing within the cabin, and where the input wants it. Both are relative
    // to straight ahead, so zero is the ordinary forward view.
    f32 LookYaw {};
    f32 LookPitch {};
    f32 TargetYaw {};
    f32 TargetPitch {};
    f32 IdleTime {};

    i32 PrevMouseX {};
    i32 PrevMouseY {};
    b32 HasMouse {};

    // What the state was last driven by. MakeActive is closed and does real work - it hands the car
    // model to DashActivated or Deactivate - so it is left alone rather than overridden for the sake
    // of a reset hook. Watching the car instead is better anyway: the camera being reselected is not
    // what invalidates this state, the car being a different car is.
    const mmCar* Car {};

    // Tunables, read once on first use. There is no Init() hook to hang them off that is guaranteed
    // to run for a camera the game creates itself, so they are resolved lazily instead.
    b32 Ready {};
    b32 LookEnabled {};
    f32 YawScale {};
    f32 PitchScale {};
    f32 SmoothRate {};
    f32 RecenterDelay {};
    f32 BobShift {};
    f32 BobSway {};
};

static PovHeadState PovHead;

static f32 PovBlend(f32 rate, f32 delta)
{
    return 1.0f - std::exp(-rate * delta);
}

static void PovEnsureReady()
{
    if (PovHead.Ready)
        return;

    PovHead.Ready = true;

    PovHead.LookEnabled = PARAM_povlook.get_or(true);

    f32 sensitivity = PARAM_povlooksens.get_or(0.0035f);

    // Negative by default for the same reason the orbital camera's are: raw mouse motion drives both
    // axes the wrong way round for a view that turns with the mouse rather than being dragged by it.
    PovHead.YawScale = PARAM_povlookinvertx.get_or(false) ? sensitivity : -sensitivity;
    PovHead.PitchScale = PARAM_povlookinverty.get_or(false) ? sensitivity : -sensitivity;

    PovHead.SmoothRate = PARAM_povlooksmooth.get_or(16.0f);
    PovHead.RecenterDelay = PARAM_povlookrecenter.get_or(1.2f);

    PovHead.BobShift = PARAM_povbob.get_or(1.0f);
    PovHead.BobSway = PARAM_povbobsway.get_or(1.0f);

    PovHead.Shake.Init();
}

// Rotates an orthonormal basis about one of its OWN axes.
//
// Doing it this way is what keeps this code free of any assumption about which of m0/m1/m2 is
// forward, or which way round the engine's camera convention runs. UpdatePOV has already built a
// correct camera matrix; these only turn it, and turning a basis about its own axes needs to know
// nothing about what those axes mean.
static void PovSpin(Vector3& a, Vector3& b, f32 angle)
{
    const f32 c = std::cos(angle);
    const f32 s = std::sin(angle);

    const Vector3 a0 = a;

    a = (a0 * c) + (b * s);
    b = (b * c) - (a0 * s);
}

void PovCamCS::UpdateInput()
{}

// The head bob and the free look, on top of the camera the original built.
//
// The original Update() is reproduced in full below - it is UpdatePOV() followed by clearing
// OneShot, and nothing else - so this adds to that camera rather than replacing it. That matters:
// the seat offset, MakeActive's dash-model handling, Reset, and whatever mmPlayer::IsPOV() keys off
// are all closed and all still in force. An earlier attempt at this put a whole new camera class in
// the player's CarCams[] instead, which reached none of that and, as it turned out, was never
// selected at all - the camera the player actually cycles to is this one, so this is where the head
// has to go.
void PovCamCS::Update()
{
    UpdatePOV();

    // Straight from the original (game.asm ~309791). It clears on any nonzero value, so the test is
    // a compiler artifact rather than a condition worth keeping.
    OneShot = 0;

    PovEnsureReady();

    // A new race, a new car, and none of the running state means anything - the first frame's
    // derivatives would otherwise be taken against the previous car's velocity and land as a
    // collision.
    if (PovHead.Car != Car)
    {
        PovHead.Car = Car;

        PovHead.Shake.Reset();

        PovHead.LookYaw = 0.0f;
        PovHead.LookPitch = 0.0f;
        PovHead.TargetYaw = 0.0f;
        PovHead.TargetPitch = 0.0f;
        PovHead.IdleTime = 0.0f;
        PovHead.HasMouse = false;
    }

    const f32 delta = Sim()->GetUpdateDelta();

    // Always re-read the accumulator, even when the input is being thrown away, so that resuming
    // never applies everything that happened while the camera was not listening.
    i32 mouse_x = PovHead.PrevMouseX;
    i32 mouse_y = PovHead.PrevMouseY;

    if (eqEventHandler* queue = eqEventHandler::SuperQ)
    {
        mouse_x = queue->GetMouseVirtualX();
        mouse_y = queue->GetMouseVirtualY();

        // Free look needs mouse travel past the window's edge in windowed mode - see
        // SDLEventHandler::BeginTracking.
        if (PovHead.LookEnabled && !Sim()->IsPaused())
            queue->BeginTracking();
    }

    // First frame in this camera: adopt the mouse position without acting on it, so a stretch spent
    // in another view does not arrive as one accumulated swing.
    if (!PovHead.HasMouse)
    {
        PovHead.PrevMouseX = mouse_x;
        PovHead.PrevMouseY = mouse_y;
        PovHead.HasMouse = true;
    }

    const i32 step_x = std::clamp(mouse_x - PovHead.PrevMouseX, -PovMaxMouseStep, PovMaxMouseStep);
    const i32 step_y = std::clamp(mouse_y - PovHead.PrevMouseY, -PovMaxMouseStep, PovMaxMouseStep);

    PovHead.PrevMouseX = mouse_x;
    PovHead.PrevMouseY = mouse_y;

    if (!Sim()->IsPaused() && (delta > 0.0f))
    {
        f32 mouse_turn_rate = 0.0f;

        if (PovHead.LookEnabled)
        {
            mouse_turn_rate = std::fabs(static_cast<f32>(step_x) * PovHead.YawScale) / delta;

            if ((step_x != 0) || (step_y != 0))
            {
                PovHead.TargetYaw = std::clamp(PovHead.TargetYaw + (static_cast<f32>(step_x) * PovHead.YawScale),
                    -PovLookYawLimit, PovLookYawLimit);
                PovHead.TargetPitch = std::clamp(PovHead.TargetPitch + (static_cast<f32>(step_y) * PovHead.PitchScale),
                    PovLookPitchMin, PovLookPitchMax);

                PovHead.IdleTime = 0.0f;
            }
            else
            {
                PovHead.IdleTime += delta;

                if ((PovHead.RecenterDelay > 0.0f) && (PovHead.IdleTime >= PovHead.RecenterDelay))
                {
                    const f32 recenter = PovBlend(PovRecenterRate, delta);

                    PovHead.TargetYaw -= PovHead.TargetYaw * recenter;
                    PovHead.TargetPitch -= PovHead.TargetPitch * recenter;
                }
            }

            if (PovHead.SmoothRate > 0.0f)
            {
                const f32 blend = PovBlend(PovHead.SmoothRate, delta);

                PovHead.LookYaw += (PovHead.TargetYaw - PovHead.LookYaw) * blend;
                PovHead.LookPitch += (PovHead.TargetPitch - PovHead.LookPitch) * blend;
            }
            else
            {
                PovHead.LookYaw = PovHead.TargetYaw;
                PovHead.LookPitch = PovHead.TargetPitch;
            }
        }

        PovHead.Shake.Update(Car, CarMatrix, delta, mouse_turn_rate);
    }

    const f32 shake = PovHead.Shake.Strength();

    // Rotation, applied to the camera the original built rather than composed from scratch. Yaw
    // about the view's own up, then pitch about the resulting right, then roll about the resulting
    // forward - which keeps the horizon level as the head turns, and inherits the car's own roll and
    // pitch for free, because UpdatePOV already put them in the matrix this is turning.
    const f32 yaw =
        PovHead.LookYaw + ((shake > 0.0f) ? (PovHead.Shake.YawNoise() * shake * PovBobYaw * PovHead.BobSway) : 0.0f);
    const f32 pitch = PovHead.LookPitch +
        ((shake > 0.0f) ? (PovHead.Shake.PitchNoise() * shake * PovBobPitch * PovHead.BobSway) : 0.0f);
    const f32 roll = (shake > 0.0f) ? (PovHead.Shake.RollNoise() * shake * PovBobRoll * PovHead.BobSway) : 0.0f;

    if (yaw != 0.0f)
        PovSpin(camera_.m2, camera_.m0, yaw);

    if (pitch != 0.0f)
        PovSpin(camera_.m2, camera_.m1, pitch);

    if (roll != 0.0f)
        PovSpin(camera_.m0, camera_.m1, roll);

    // The bob proper, along the CAR's axes rather than the head's, because what moves is the body in
    // the seat. Applying it along the view would swing the eye sideways as the player looked around,
    // which is the one thing a head bob must never do.
    if ((shake > 0.0f) && CarMatrix)
    {
        camera_.m3 += CarMatrix->m0 * (PovHead.Shake.RightNoise() * shake * PovBobRight * PovHead.BobShift);
        camera_.m3 += CarMatrix->m1 * (PovHead.Shake.UpNoise() * shake * PovBobUp * PovHead.BobShift);
        camera_.m3 += CarMatrix->m2 * (PovHead.Shake.ForwardNoise() * shake * PovBobForward * PovHead.BobShift);
    }

    // The lean. Negated because gaining speed along the car's nose throws the head the other way -
    // back into the seat - and braking does the reverse.
    if (CarMatrix)
        camera_.m3 -= CarMatrix->m2 * (PovHead.Shake.AccelSigned * PovLeanAccel * PovHead.BobShift);
}
