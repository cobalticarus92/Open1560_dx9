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

#include "dx9meshcache.h"

#include "agi/vertex.h"

#include "dx9rsys.h"

#include "dx9_windows.h"

#include <algorithm>
#include <cstring>
#include <mutex>

define_dummy_symbol(agidx9_dx9meshcache);

// HOW IT IS BUILT
//
// Everything lives in fixed tables and the process heap, never the engine's allocator. The build this
// replaces documents why at length (the ARTS_ALLOCA note in agiMeshSet::DrawNativeTransform): calling
// the game's allocator from inside a draw has corrupted its simulation heap before. The process heap
// is a separate heap entirely, and nothing the arena reset between races frees - so a cached mesh
// also cannot be pulled out from under the cache when the city it came from is unloaded; it simply
// stops being drawn and ages out.
//
// A mesh is kept on its SECOND sighting, not its first. A key seen once is remembered in a small
// direct-mapped table and nothing more. Geometry that changes every frame - a car lit on the CPU
// whose colours move with the lights, a dented body settling - mints a new key per draw and would
// otherwise fill the cache with meshes that are never drawn again, each one a buffer upload. Seen
// twice means it is either static or instanced, and both are exactly what the cache is for: a street
// of identical lamp posts is one entry, drawn many times.
//
// Nothing is evicted during a frame. DrawNativeTransform holds the pointer Find or Store returned for
// the length of its draw, and the renderer reads it in MeshWorld, so an entry must not disappear
// mid-frame; eviction runs at BeginFrame instead, least recently drawn first, and only when the cache
// is over budget. A frame that fills the cache simply draws the rest the old way.

static mem::cmd_param PARAM_d3d9_meshcache {"d3d9meshcache", "Keep static world meshes on the GPU between frames"};
static mem::cmd_param PARAM_d3d9_meshcache_mb {"d3d9meshcachemb", "World mesh cache budget, in MB"};

namespace
{
    struct CacheEntry
    {
        u64 Key;
        agiNativeCachedMesh Mesh;

        // Mesh's three arrays, in one process-heap block.
        void* Block;
        u32 Bytes;

        u32 LastFrame;

        IDirect3DVertexBuffer9* Vb;
        IDirect3DIndexBuffer9* Ib;
        bool BuffersFailed;
        bool Live;
    };

    constexpr u32 kMaxEntries = 8192;

    // Open addressing, at most half full so a probe stays short. Rebuilt whole after an eviction
    // rather than supporting deletion.
    constexpr u32 kTableSize = kMaxEntries * 2;
    constexpr u32 kSeenSize = 16384;
    constexpr i32 kEmpty = -1;

    // How many frames an entry must have gone undrawn before it is a candidate at all. An entry is
    // only removed when the cache is over budget, so this is a floor, not a lifetime: a city block
    // behind the camera stays cached until the space is wanted.
    constexpr u32 kMinIdleFrames = 30;
} // namespace

static CacheEntry s_entries[kMaxEntries] {};
static u32 s_free[kMaxEntries] {};
static u32 s_free_count = 0;
static i32 s_table[kTableSize] {};
static u64 s_seen[kSeenSize] {};
static bool s_initialised = false;

static u32 s_frame = 1;
static u32 s_live = 0;
static u64 s_bytes = 0;

// Since the last census line.
static u32 s_hits = 0;
static u32 s_stores = 0;
static u32 s_full = 0;
static u32 s_evicted = 0;
static u32 s_buffer_failures = 0;

// The device is created D3DCREATE_MULTITHREADED and DrawNativeTransform is reached on more than one
// thread. std::mutex has a constexpr constructor, so there is no static-initialisation order to get
// wrong, and it constructs nothing through the engine's allocator.
static std::mutex s_lock;

using CacheLock = std::lock_guard<std::mutex>;

bool agiDX9MeshCacheEnabled()
{
    static const bool enabled = PARAM_d3d9_meshcache.get_or(true);

    return enabled;
}

static u64 BudgetBytes()
{
    static const u64 budget = static_cast<u64>(std::max(8, PARAM_d3d9_meshcache_mb.get_or(64))) * 1024u * 1024u;

    return budget;
}

static u32 Slot(u64 key, u32 size)
{
    return static_cast<u32>(key ^ (key >> 32)) & (size - 1);
}

static void InitTables()
{
    if (s_initialised)
        return;

    for (u32 i = 0; i < kTableSize; ++i)
        s_table[i] = kEmpty;

    // Handed out from the top, so entry 0 goes first.
    for (u32 i = 0; i < kMaxEntries; ++i)
        s_free[i] = kMaxEntries - 1 - i;

    s_free_count = kMaxEntries;
    s_initialised = true;
}

static CacheEntry* Lookup(u64 key)
{
    for (u32 slot = Slot(key, kTableSize);; slot = (slot + 1) & (kTableSize - 1))
    {
        const i32 index = s_table[slot];

        if (index == kEmpty)
            return nullptr;

        if (s_entries[index].Key == key)
            return &s_entries[index];
    }
}

static void Insert(u32 index)
{
    u32 slot = Slot(s_entries[index].Key, kTableSize);

    while (s_table[slot] != kEmpty)
        slot = (slot + 1) & (kTableSize - 1);

    s_table[slot] = static_cast<i32>(index);
}

static void FreeEntry(CacheEntry& entry)
{
    if (entry.Vb)
        entry.Vb->Release();

    if (entry.Ib)
        entry.Ib->Release();

    if (entry.Block)
        HeapFree(GetProcessHeap(), 0, entry.Block);

    s_bytes -= entry.Bytes;
    --s_live;

    entry = {};

    s_free[s_free_count++] = static_cast<u32>(&entry - s_entries);
}

static void RebuildTable()
{
    for (u32 i = 0; i < kTableSize; ++i)
        s_table[i] = kEmpty;

    for (u32 i = 0; i < kMaxEntries; ++i)
    {
        if (s_entries[i].Live)
            Insert(i);
    }
}

const agiNativeCachedMesh* agiDX9MeshCacheFind(u64 key)
{
    CacheLock lock(s_lock);

    if (!s_initialised)
        return nullptr;

    CacheEntry* entry = Lookup(key);

    if (entry == nullptr)
        return nullptr;

    entry->LastFrame = s_frame;
    ++s_hits;

    return &entry->Mesh;
}

const agiNativeCachedMesh* agiDX9MeshCacheStore(u64 key, const agiWorldVtx* vertices, u32 vertex_count,
    const u16* indices, u32 index_count, const agiNativeMeshBatch* batches, u32 batch_count)
{
    if ((vertex_count == 0) || (index_count == 0) || (batch_count == 0))
        return nullptr;

    CacheLock lock(s_lock);

    InitTables();

    // Another thread may have stored it between this thread's Find and here.
    if (CacheEntry* existing = Lookup(key))
    {
        existing->LastFrame = s_frame;
        return &existing->Mesh;
    }

    u64& seen = s_seen[Slot(key, kSeenSize)];

    if (seen != key)
    {
        seen = key;
        return nullptr;
    }

    const u32 vertex_bytes = vertex_count * sizeof(agiWorldVtx);
    const u32 index_bytes = ((index_count * sizeof(u16)) + 3u) & ~3u;
    const u32 batch_bytes = batch_count * sizeof(agiNativeMeshBatch);
    const u32 bytes = vertex_bytes + index_bytes + batch_bytes;

    // Full, or over budget: the rest of this frame draws the old way, and BeginFrame makes room.
    if ((s_free_count == 0) || (s_live >= kMaxEntries / 2 + kMaxEntries / 4) || (s_bytes + bytes > BudgetBytes()))
    {
        ++s_full;
        return nullptr;
    }

    u8* block = static_cast<u8*>(HeapAlloc(GetProcessHeap(), 0, bytes));

    if (block == nullptr)
    {
        ++s_full;
        return nullptr;
    }

    std::memcpy(block, vertices, vertex_bytes);
    std::memcpy(block + vertex_bytes, indices, index_count * sizeof(u16));
    std::memcpy(block + vertex_bytes + index_bytes, batches, batch_bytes);

    const u32 index = s_free[--s_free_count];
    CacheEntry& entry = s_entries[index];

    entry = {};
    entry.Key = key;
    entry.Block = block;
    entry.Bytes = bytes;
    entry.LastFrame = s_frame;
    entry.Live = true;

    entry.Mesh.Vertices = reinterpret_cast<agiWorldVtx*>(block);
    entry.Mesh.VertexCount = vertex_count;
    entry.Mesh.Indices = reinterpret_cast<u16*>(block + vertex_bytes);
    entry.Mesh.IndexCount = index_count;
    entry.Mesh.Batches = reinterpret_cast<agiNativeMeshBatch*>(block + vertex_bytes + index_bytes);
    entry.Mesh.BatchCount = batch_count;
    entry.Mesh.RendererData = &entry;

    Insert(index);

    ++s_live;
    s_bytes += bytes;
    ++s_stores;

    return &entry.Mesh;
}

bool agiDX9MeshCacheBuffers(
    IDirect3DDevice9* device, const agiNativeCachedMesh& mesh, IDirect3DVertexBuffer9** vb, IDirect3DIndexBuffer9** ib)
{
    CacheEntry* entry = static_cast<CacheEntry*>(mesh.RendererData);

    if (entry == nullptr)
        return false;

    CacheLock lock(s_lock);

    if (entry->BuffersFailed || !entry->Live)
        return false;

    if (entry->Vb == nullptr)
    {
        const UINT vertex_bytes = mesh.VertexCount * sizeof(agiWorldVtx);
        const UINT index_bytes = mesh.IndexCount * sizeof(u16);

        // MANAGED, so a device Reset - the menu/race transition, a resize, a lost device - does not
        // take the buffers with it. The runtime restores them itself.
        IDirect3DVertexBuffer9* new_vb = nullptr;
        IDirect3DIndexBuffer9* new_ib = nullptr;

        bool ok = SUCCEEDED(device->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY,
                      D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1, D3DPOOL_MANAGED, &new_vb, nullptr)) &&
            SUCCEEDED(device->CreateIndexBuffer(
                index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &new_ib, nullptr));

        void* data = nullptr;

        if (ok && SUCCEEDED(new_vb->Lock(0, vertex_bytes, &data, 0)))
        {
            std::memcpy(data, mesh.Vertices, vertex_bytes);
            new_vb->Unlock();
        }
        else
        {
            ok = false;
        }

        if (ok && SUCCEEDED(new_ib->Lock(0, index_bytes, &data, 0)))
        {
            std::memcpy(data, mesh.Indices, index_bytes);
            new_ib->Unlock();
        }
        else
        {
            ok = false;
        }

        if (!ok)
        {
            if (new_vb)
                new_vb->Release();

            if (new_ib)
                new_ib->Release();

            entry->BuffersFailed = true;
            ++s_buffer_failures;
            return false;
        }

        entry->Vb = new_vb;
        entry->Ib = new_ib;
    }

    *vb = entry->Vb;
    *ib = entry->Ib;

    return true;
}

void agiDX9MeshCacheBeginFrame()
{
    CacheLock lock(s_lock);

    ++s_frame;

    if (!s_initialised)
        return;

    const u64 budget = BudgetBytes();
    const u32 max_live = kMaxEntries / 2;

    // Eviction frees down to a low-water mark rather than to the limit, so a cache running at its
    // budget does not evict a handful of entries, and rebuild its table, every frame.
    const u64 target_bytes = budget - budget / 4;

    if ((s_bytes <= target_bytes) && (s_live <= max_live))
        return;

    // Oldest first, without sorting: widen the idle window until enough has gone. Each pass is a
    // linear walk of a table that is small next to a frame's draw work, and this runs rarely.
    u32 evicted = 0;

    for (u32 idle = 1u << 12; ((s_bytes > target_bytes) || (s_live > max_live)) && (idle >= kMinIdleFrames); idle /= 2)
    {
        for (u32 i = 0; i < kMaxEntries; ++i)
        {
            CacheEntry& entry = s_entries[i];

            if (!entry.Live || (s_frame - entry.LastFrame < idle))
                continue;

            FreeEntry(entry);
            ++evicted;

            if ((s_bytes <= target_bytes) && (s_live <= max_live))
                break;
        }
    }

    if (evicted)
    {
        s_evicted += evicted;
        RebuildTable();

        // A released buffer's address can be reused by the next one created, and the rasterizer
        // skips a SetStreamSource it believes redundant - see agiDX9ForgetStreams.
        agiDX9ForgetStreams();
    }
}

void agiDX9MeshCacheReleaseAll()
{
    CacheLock lock(s_lock);

    if (!s_initialised)
        return;

    // Unbind first. The device holds a reference to whatever is bound, which would keep the last
    // mesh's buffers alive, and pointing at the old city, until something else was bound.
    for (u32 i = 0; i < kMaxEntries; ++i)
    {
        if (s_entries[i].Vb)
        {
            IDirect3DDevice9* device = nullptr;

            if (SUCCEEDED(s_entries[i].Vb->GetDevice(&device)) && device)
            {
                device->SetStreamSource(0, nullptr, 0, 0);
                device->SetIndices(nullptr);
                device->Release();
            }

            break;
        }
    }

    for (u32 i = 0; i < kMaxEntries; ++i)
    {
        if (s_entries[i].Live)
            FreeEntry(s_entries[i]);
    }

    for (u32 i = 0; i < kSeenSize; ++i)
        s_seen[i] = 0;

    RebuildTable();
    agiDX9ForgetStreams();
}

void agiDX9MeshCacheLogStats(u32 frame, u32 cached_draws, u32 world_draws)
{
    if (!agiDX9MeshCacheEnabled())
        return;

    u32 hits;
    u32 stores;
    u32 full;
    u32 evicted;
    u32 failures;
    u32 live;
    u64 bytes;

    {
        CacheLock lock(s_lock);

        hits = s_hits;
        stores = s_stores;
        full = s_full;
        evicted = s_evicted;
        failures = s_buffer_failures;
        live = s_live;
        bytes = s_bytes;

        s_hits = 0;
        s_stores = 0;
        s_full = 0;
        s_evicted = 0;
    }

    Displayf("DX9 MESHCACHE: frame=%u world draws=%u from buffers/%u | since last: hits=%u stored=%u no-room=%u "
             "evicted=%u | %u meshes, %.1f/%u MB%s",
        frame, cached_draws, world_draws, hits, stores, full, evicted, live,
        static_cast<double>(bytes) / (1024.0 * 1024.0), static_cast<u32>(BudgetBytes() / (1024u * 1024u)),
        failures ? " | BUFFER CREATION FAILED" : "");
}
