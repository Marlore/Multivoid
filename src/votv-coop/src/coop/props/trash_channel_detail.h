// coop/props/trash_channel_detail.h -- INTERNAL seam between trash_channel.cpp
// (the ctx/carry-latch/land-settle/birth-certificate core) and
// trash_grab_intent.cpp (the client-initiated grab/throw intent lane).
//
// Sibling-internal header (the prop_element_tracker_detail.h precedent) -- NOT
// part of the public coop/ include surface; only the two TUs above include it.
//
// The HELD_BY registry (eid -> holder peer slot, the door holdOpen_ analog) is
// OWNED by trash_grab_intent.cpp; the core never touches the map directly --
// TickCarry / the birth prune / OnDisconnect go through these three ops.
// All game-thread-only, like every trash_channel entry point.

#pragma once

#include <cstdint>

namespace coop::trash_channel {

// Is `eid` currently held by ANY peer's puppet (a client-initiated grab)?
bool HeldByAny(uint32_t eid);

// Drop `eid`'s HELD_BY record if present (land commit / dead-close / rest-close
// end the hold; the eid becomes re-grabbable). Idempotent.
void ClearHeldBy(uint32_t eid);

// Gross reset of the intent lane (net disconnect): HELD_BY + the client-side
// pending-grab / carry toggles.
void ResetIntentState();

// FIX: Clear smear heal debounce state across disconnect/reconnect.
// Prevents stale 5s debounce from blocking legitimate incremental spawns after reconnect.
void ResetSmearHealState();

// Close the carry latch for an eid (called from throw intent so the holder
// can pick up trash again while the clump is flying).
void CloseCarryForEid(uint32_t eid);

}  // namespace coop::trash_channel
