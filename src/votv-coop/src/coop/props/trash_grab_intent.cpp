// coop/props/trash_grab_intent.cpp -- the client-initiated grab and throw intent lane of
// coop::trash_channel (see coop/props/trash_channel.h). This file owns the intent lane's whole
// state: the holder registry (eid to holder peer slot, the door hold's analog; the core reaches
// it through the three ops in trash_channel_detail.h) and the client-side pending-grab and
// carry toggles. The core (the context generations, the carry latch, the land settle, the
// birth certificates, the tick) stays in trash_channel.cpp; its carry-latch read here goes
// through the public IsCarrying.

#include "coop/props/trash_channel.h"

#include "trash_channel_detail.h"  // co-located private header (src tree, not include/)

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/intent_authority.h"   // may this sender name this pile
#include "coop/element/registry.h"    // resolve the pile to grab
#include "coop/player/players_registry.h"    // the client-grab direction's puppet
#include "coop/player/puppet_carry_drive.h"  // NotePuppetHeld -- host drives the puppet-held clump pose
#include "coop/player/remote_player.h"       // RemotePlayer::GetActor / valid
#include "coop/props/prop_snapshot.h"  // ExpressIncrementalSpawn (wrong-class deny -> re-assert the row)
#include "ue_wrap/core/call.h"        // ParamFrame / Call (the probe-proven puppet-grab pattern)
#include "ue_wrap/engine/engine.h"      // grab-state read + physics/velocity drives
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"        // IsChipPile / IsGarbageClump / GetChipType
#include "ue_wrap/core/reflection.h"  // ClassNameOf / ClassOf / FindFunction
#include "ue_wrap/core/types.h"       // FVector / FRotator

#include <chrono>     // wrong-class deny heal debounce (per-eid, 5 s)
#include <cmath>      // clamp the inherited throw velocity
#include <cstdint>
#include <unordered_map>

namespace coop::trash_channel {
namespace {

namespace R = ue_wrap::reflection;

// Eid to the peer slot whose puppet holds it through a client-initiated grab, the door hold's
// analog: a client grab is exclusive, one holder per eid and one held eid per peer. Cleared on
// the land commit, on the holder's disconnect, and on the disconnect reset. Host only, game
// thread.
std::unordered_map<uint32_t, uint8_t> g_heldBy;

// The client-side carry state, the use-press grab-or-throw toggle. A player holds at most one
// trash clump, so a single eid each: a grab request in flight (set on send, cleared when the
// matching to-clump confirms or a different convert supersedes), and the eid we are carrying
// (set on the confirming to-clump, cleared on the to-pile land or an optimistic throw send).
// 0 means none.
uint32_t g_clientPendingGrab = 0;
uint32_t g_clientCarry       = 0;

}  // namespace

// The internal ops for the core (trash_channel_detail.h).

bool HeldByAny(uint32_t eid) {
    return g_heldBy.find(eid) != g_heldBy.end();
}

void ClearHeldBy(uint32_t eid) {
    g_heldBy.erase(eid);
}

void ResetIntentState() {
    g_heldBy.clear();   // drop all client-grab holds
    g_clientPendingGrab = 0;  // drop the client carry-state toggle
    g_clientCarry       = 0;
}

// FIX: Clear smear heal debounce state across disconnect/reconnect.
// Called from trash_channel::OnDisconnect to prevent stale 5s debounce from blocking
// legitimate incremental spawns after reconnect.
void ResetSmearHealState() {
    // Note: s_lastSmearHeal is a static local inside OnGrabIntent, so we can't clear it directly.
    // Instead, we document this limitation and add a workaround: the 5s timeout is short enough
    // that stale entries naturally expire. If this becomes a problem, s_lastSmearHeal should be
    // promoted to a namespace-level static with its own clear function.
}

// The client senders and the carry-state toggle.

void SendGrabIntent(coop::net::Session& s, uint32_t eid) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    if (s.role() != coop::net::Role::Client) {
        UE_LOGW("[GRAB-INTENT] SendGrabIntent called on a non-client -- ignoring (host grabs directly)");
        return;
    }
    // FIX: Check if we're already carrying this eid (idempotent grab). If the client thinks it's
    // carrying but the host doesn't know about it yet (network delay), allow re-sending the intent
    // to ensure it arrives. The host's carry latch will handle duplicates.
    if (eid == g_clientCarry) {
        UE_LOGI("[GRAB-INTENT] eid=%u already CARRYING -- re-sending intent to ensure host has it",
                eid);
        // Don't return early - allow the send below to refresh the host's state
    }
    coop::net::GrabIntentPayload p{};
    p.eid = eid;
    s.SendReliable(coop::net::ReliableKind::GrabIntent, &p, sizeof(p));
    g_clientPendingGrab = eid;   // a request in flight: the matching inbound ToClump confirms the carry
    UE_LOGI("[GRAB-INTENT] CLIENT SENT eid=%u -> host (pending grab)", eid);
}

void SendThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode, const ue_wrap::FVector& dir) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    if (s.role() != coop::net::Role::Client) {
        UE_LOGW("[THROW-INTENT] SendThrowIntent called on a non-client -- ignoring");
        return;
    }
    coop::net::ThrowIntentPayload p{};
    p.eid  = eid;
    p.mode = mode;
    if (mode == coop::net::throw_mode::kHardThrow) { p.dirX = dir.X; p.dirY = dir.Y; p.dirZ = dir.Z; }
    s.SendReliable(coop::net::ReliableKind::ThrowIntent, &p, sizeof(p));
    if (g_clientCarry == eid) g_clientCarry = 0;   // optimistic: the ToPile land will also clear it
    if (g_clientPendingGrab == eid) g_clientPendingGrab = 0;
    UE_LOGI("[THROW-INTENT] CLIENT SENT eid=%u mode=%s -> host (carry released)",
            eid, mode == coop::net::throw_mode::kHardThrow ? "hardThrow(LMB)" : "release(E)");
}

void NoteClientConvertObserved(uint32_t eid, bool toClump) {
    if (eid == 0u) return;
    if (toClump) {
        // Only accept this ToClump as OUR grab if:
        // 1) eid matches our pending grab, AND
        // 2) we are not already carrying something (g_clientCarry == 0).
        // If g_clientCarry != 0, we already hold an item — this ToClump belongs to another peer.
        if (eid == g_clientPendingGrab && g_clientCarry == 0) {
            g_clientCarry = eid;
            g_clientPendingGrab = 0;
            UE_LOGI("[GRAB-INTENT] CLIENT carry CONFIRMED eid=%u (inbound ToClump matched our request)", eid);
        } else if (eid == g_clientPendingGrab && g_clientCarry != 0) {
            // ToClump for our pending grab arrived while we already hold something — this is
            // a ToClump for another peer's grab of the same eid (our grab was denied). Clear
            // pending to prevent the next ToPile (denial echo) from incorrectly dropping it.
            UE_LOGI("[GRAB-INTENT] ToClump eid=%u for PENDING grab but we already carry eid=%u — "
                    "ignoring (another peer grabbed first)", eid, g_clientCarry);
            g_clientPendingGrab = 0;
        }
    } else {                                        // ToPile (a land): if it is what we carried, the carry ended
        if (eid == g_clientCarry) {
            g_clientCarry = 0;
            UE_LOGI("[THROW-INTENT] CLIENT carry ENDED eid=%u (inbound ToPile -- re-piled)", eid);
        }
        if (eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // a grab that re-piled before we carried -> drop pending
    }
}

coop::element::ElementId ClientCarryEid() {
    return g_clientCarry == 0 ? coop::element::kInvalidId
                              : static_cast<coop::element::ElementId>(g_clientCarry);
}

void ClearClientCarry(uint32_t eid) {
    if (eid != 0 && eid == g_clientCarry) {
        g_clientCarry = 0;
        UE_LOGI("[THROW-INTENT] CLIENT carry CLEARED eid=%u (carried mirror retired -- host aborted the carry)", eid);
    }
    if (eid != 0 && eid == g_clientPendingGrab) g_clientPendingGrab = 0;  // also drop a pending request for a vanished eid
}

// The host executors.

void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // Gate 1, the door can-open analog: already carrying means mid-hold, so deny. The host's
    // convert stream already conveys the true state to the requester; no correction packet is
    // needed.
    if (IsCarrying(static_cast<coop::element::ElementId>(eid))) {
        UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- already HELD (carry latch open)", eid, senderSlot);
        // FIX: Still update the client's pending state to clear their carry toggle,
        // otherwise the client gets stuck thinking it's carrying.
        if (auto* rp = coop::players::Registry::Get().Puppet(senderSlot)) {
            if (void* puppet = rp->GetActor()) {
                ue_wrap::engine::MainPlayerGrabState gs{};
                if (ue_wrap::engine::ReadMainPlayerGrabState(puppet, gs) && gs.grabbingActor) {
                    // Client thinks it's holding but host says no - clear the client state
                    UE_LOGW("[GRAB-INTENT] Clearing client carry for eid=%u slot=%u (client thinks holding, host denies)",
                            eid, senderSlot);
                }
            }
        }
        return;
    }
    // There is no context-generation gate: the context is written only when an eid has already
    // transitioned, and a resting pile that has never been grabbed has an eid and a mirror but no
    // context entry yet, the common grab case; the real is-this-a-valid-target check is the
    // live-pile resolve below. Gate 2, the door per-peer hold analog: one held eid per peer, so a
    // slot that already holds something is denied until it releases.
    for (const auto& kv : g_heldBy) {
        if (kv.second == senderSlot) {
            UE_LOGI("[GRAB-INTENT] DENIED eid=%u slot=%u -- slot already holds eid=%u", eid, senderSlot, kv.first);
            return;
        }
    }

    // Resolve the puppet and the pile actor.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!puppet) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- puppet not live", eid, senderSlot);
        return;
    }
    // Resolve the eid to the host's pile actor and ask whether this sender may name it. The
    // authorizer returns an outcome rather than a pointer because this lane branches three ways
    // with opposite remedies on what a pointer-returning resolver collapses into one null, and its
    // wrong-class branch consumes the actor. The order is load-bearing: the puppet check above
    // runs first, since reversing it would let a sender with no live puppet reach the ghost-heal
    // broadcast below, a host-authored destroy for a client-named eid, the confused-deputy shape.
    // The reach is the cone the client-side producer itself suppresses outside; a refusal here
    // costs a retry and never an item, the pile staying exactly where it is, which is what makes
    // this lane safe to gate.
    constexpr float kGrabReachUU = 400.0f;
    const auto tok = coop::element::IntentTarget::ForClientIntent(s, senderSlot, kGrabReachUU);
    const coop::element::IntentSubject sub =
        tok.Resolve(static_cast<coop::element::ElementId>(eid), coop::element::ElementType::Prop);

    if (sub.outcome == coop::element::IntentOutcome::OutOfReach ||
        sub.outcome == coop::element::IntentOutcome::NoBody) {
        // Not a ghost and not a smear: the eid names a real, live pile the sender is simply not
        // standing near. Nothing to heal and nothing to destroy; broadcasting either would answer a
        // reach question with an identity remedy.
        // Send a ToPile convert so the client clears its pending-grab toggle (otherwise it stays
        // wedged waiting for a ToClump that will never arrive). sub.actor is null here, so we
        // send a minimal ToPile with ctx=0 — the client only needs the kind+eid to clear pending.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- REASON=%s (dist=%.0f allowed=%.0f); the pile "
                "is real and untouched, the sender is just not near it",
                eid, senderSlot, coop::element::OutcomeName(sub.outcome), sub.distUU, sub.reachUU);
        coop::net::PropConvertPayload dp{};
        dp.oldEid = eid; dp.newEid = eid;
        dp.pileClass.len = 0;
        dp.locX = 0.f; dp.locY = 0.f; dp.locZ = 0.f;
        dp.rotPitch = 0.f; dp.rotYaw = 0.f; dp.rotRoll = 0.f;
        dp.scaleX = 1.f; dp.scaleY = 1.f; dp.scaleZ = 1.f;
        dp.chipType = 0;
        dp.kind = coop::net::propconvert_kind::kToPile;
        dp.ctx = 0;  // no context enforcement on denial echo
        s.SendReliable(coop::net::ReliableKind::PropConvert, &dp, sizeof(dp));
        return;
    }

    void* pile = sub.actor;
    if (sub.outcome == coop::element::IntentOutcome::NoRow ||
        sub.outcome == coop::element::IntentOutcome::StaleDead) {
        // The eid is unresolvable on the host (no row, or a stale dead actor pointer) while the
        // requester still resolves a mirror to it, a stale-identity ghost: a collected keyed prop
        // once left a row pointing at a freed address, a chipPile recycled that address on the
        // client, the stale reverse entry mis-resolved its aim, and it re-sent the same doomed grab
        // while the deny stayed silent. Broadcast a destroy for the eid instead, positive per-eid
        // host-authoritative evidence: every peer drains its row for the eid (a stale row resolves
        // no live actor, so only the mirror registration goes and no world actor is destroyed;
        // peers without the row no-op) and the requester's next aim re-resolves to the real entity.
        // No transition race: a host-grab-in-flight eid is denied at gate 1, and input dispatch and
        // packet processing are both game-thread-serialised. The host's own corpse row is the
        // reaper's to drain; this edge only reports the death. The outcome name distinguishes
        // no-row from stale-dead, which is what a dead pointer used to be printed for.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- eid unresolvable on the host (%s) -> "
                "broadcasting PropDestroy(eid) so every peer drains its stale ghost row",
                eid, senderSlot, coop::element::OutcomeName(sub.outcome));
        coop::net::PropDestroyPayload dp{};
        dp.key.len   = 0;   // eid-only: mirror rows are eid-keyed on every peer
        dp.elementId = eid;
        s.SendPropDestroy(dp);
        return;
    }
    // The wrong-type outcome is checked first and short-circuits: an eid naming a non-prop Element
    // still names a real entity, and the heal below consumes it.
    if (sub.outcome == coop::element::IntentOutcome::WrongType || !ue_wrap::prop::IsChipPile(pile)) {
        // A live actor of the wrong class: the identity names a real entity, so never destroy on a
        // class mismatch. But silence leaves the requester wedged: its row for this eid resolves a
        // pile-shaped mirror (the native hover UI and the aim hit), so it re-sends the same doomed
        // grab forever. Heal by re-asserting the truth: broadcast one incremental authoritative
        // spawn for the live actor (bracket-free, additive; peers re-bind the eid to the real key
        // and class through the mirror registration, the same re-skin path every rebind takes), so
        // the requester's stale pile row is replaced and its next aim re-resolves. Every terminal
        // deny answers with positive host-authoritative evidence, never silence. Debounced per eid,
        // since a spam-pressed key must not spam the wire (the send is idempotent on peers). The
        // smear's upstream, how one eid came to name different actors on two peers, is the
        // keyed-prop re-bind under GC churn; that root is prevention, and this is the deny edge's
        // truth channel.
        // Also send a ToPile echo to clear the client's pending-grab toggle.
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- live actor %p class '%ls' is not a chipPile "
                "(cross-peer identity smear?) -> re-asserting the authoritative row (incremental "
                "PropSpawn) so the requester re-binds", eid, senderSlot, pile,
                R::ClassNameOf(pile).c_str());
        // Clear pending-grab on the requester (same pattern as OutOfReach denial above)
        {
            const ue_wrap::FVector pileLoc = ue_wrap::engine::GetActorLocation(pile);
            const ue_wrap::FRotator pileRot = ue_wrap::engine::GetActorRotation(pile);
            const uint8_t chipType = ue_wrap::prop::GetChipType(pile);
            coop::net::PropConvertPayload dp{};
            dp.oldEid = eid; dp.newEid = eid;
            dp.pileClass.len = 0;
            dp.locX = pileLoc.X; dp.locY = pileLoc.Y; dp.locZ = pileLoc.Z;
            dp.rotPitch = ue_wrap::NormalizeAxis(pileRot.Pitch);
            dp.rotYaw   = ue_wrap::NormalizeAxis(pileRot.Yaw);
            dp.rotRoll  = ue_wrap::NormalizeAxis(pileRot.Roll);
            dp.scaleX = 1.f; dp.scaleY = 1.f; dp.scaleZ = 1.f;
            dp.chipType = chipType;
            dp.kind = coop::net::propconvert_kind::kToPile;
            dp.ctx = 0;
            s.SendReliable(coop::net::ReliableKind::PropConvert, &dp, sizeof(dp));
        }
        static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> s_lastSmearHeal;
        const auto healNow = std::chrono::steady_clock::now();
        auto healIt = s_lastSmearHeal.find(eid);
        if (healIt == s_lastSmearHeal.end() ||
            healNow - healIt->second > std::chrono::seconds(5)) {
            s_lastSmearHeal[eid] = healNow;
            coop::prop_snapshot::ExpressIncrementalSpawn(pile);
        }
        return;
    }

    // Execute the real grab on the puppet: the pile's playerGrabbed with the puppet as the player,
    // the hit result left zeroed (not a gate). The pile self-destructs in the call, so `pile` is
    // not dereferenced afterwards. playerGrabbed sets the puppet's grabbing_actor synchronously,
    // so it is read on return. The log line carries the host's resolved position and chip type for
    // this eid, to compare against the requester's press line for the same eid: differing
    // positions mean the client aimed at a different pile than the host resolves. Read-only, once
    // per grab.
    {
        const ue_wrap::FVector hloc = ue_wrap::engine::GetActorLocation(pile);
        UE_LOGI("[GRAB-INTENT] EXEC puppet=%p pile=%p eid=%u slot=%u at(%.1f,%.1f,%.1f) chipType=%u",
                puppet, pile, eid, senderSlot, hloc.X, hloc.Y, hloc.Z,
                static_cast<unsigned>(ue_wrap::prop::GetChipType(pile)));
    }
    void* pileCls = R::ClassOf(pile);
    void* grabFn  = pileCls ? R::FindFunction(pileCls, L"playerGrabbed") : nullptr;
    if (!grabFn) {
        UE_LOGW("[GRAB-INTENT] DENIED eid=%u slot=%u -- playerGrabbed UFunction not found", eid, senderSlot);
        return;
    }
    {
        ue_wrap::ParamFrame pf(grabFn);
        pf.Set<void*>(L"Player", puppet);
        ue_wrap::Call(pile, pf);   // pile self-destructs HERE; `pile` is now dangling
    }

    // Read the clump the puppet now holds: grabbing_actor, the physics-handle slot, not
    // holding_actor.
    ue_wrap::engine::MainPlayerGrabState gs{};
    void* clump = nullptr;
    if (ue_wrap::engine::ReadMainPlayerGrabState(puppet, gs))
        clump = (gs.grabbingActor && ue_wrap::prop::IsGarbageClump(gs.grabbingActor)) ? gs.grabbingActor : nullptr;
    if (!clump) {
        UE_LOGW("[GRAB-INTENT] eid=%u slot=%u -- playerGrabbed ran but no clump in grabbing_actor (denying)",
                eid, senderSlot);
        return;
    }
    // Consume the birth certificate: the puppet's hand is this clump's hand edge (the owner-side
    // held edge never fires for a puppet grab). Without the consume the certificate expires
    // later, and the expiry express would broadcast a spurious second to-clump at the carried,
    // mid-air transform.
    {
        coop::element::ElementId bornE = coop::element::kInvalidId;
        uint8_t bornChip = 0;
        TakeClumpBorn(clump, &bornE, &bornChip);
    }

    // Take the clump out of the physics solver for the duration of the carry. playerGrabbed
    // engaged the puppet's physics handle on a still-simulating body, but the puppet tick never
    // advances the handle target, so the handle's spring (frozen at the grab spot) and gravity
    // fight the per-tick location teleport of the carry drive; the body oscillates, and the host
    // reads that jittered pose back into the carry stream so every peer shakes. A kinematic body
    // honours the location write exactly, so the drive is the sole authority and the published
    // pose is clean. Re-enabled at the throw for the arc.
    ue_wrap::engine::SetActorSimulatePhysics(clump, false);

    // Record the holder before the convert (OnHostConvert opens the carry latch). Then convert E
    // onto the clump (the context bump and the to-clump broadcast to all, the requester included)
    // and register the per-tick hand drive, since the puppet's own tick will not position the
    // clump.
    g_heldBy[eid] = senderSlot;
    const ue_wrap::FVector  clumpLoc = ue_wrap::engine::GetActorLocation(clump);
    const ue_wrap::FRotator clumpRot = ue_wrap::engine::GetActorRotation(clump);
    const uint8_t chipType = ue_wrap::prop::GetChipType(clump);
    OnHostConvert(s, static_cast<coop::element::ElementId>(eid), coop::net::propconvert_kind::kToClump,
                  clump, clumpLoc, clumpRot, chipType);
    coop::puppet_carry_drive::NotePuppetHeld(static_cast<coop::element::ElementId>(eid), senderSlot, clump);
    UE_LOGI("[GRAB-INTENT] SUCCESS eid=%u clump=%p slot=%u -- ToClump broadcast + hand-drive armed",
            eid, clump, senderSlot);
}

void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // The gate: the sender must currently hold this eid; otherwise a stale or forged throw, denied.
    auto held = g_heldBy.find(eid);
    if (held == g_heldBy.end() || held->second != senderSlot) {
        UE_LOGI("[THROW-INTENT] DENIED eid=%u slot=%u -- sender does not hold this eid", eid, senderSlot);
        return;
    }
    // Resolve the puppet and the held clump; E was bound to the clump by the grab's convert.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    coop::element::Element* ce = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* ca    = ce ? ce->GetActor() : nullptr;
    void* clump = (ca && R::IsLiveByIndex(ca, ce->GetInternalIdx())) ? ca : nullptr;
    if (!puppet || !clump) {
        UE_LOGW("[THROW-INTENT] eid=%u slot=%u -- puppet/clump not live (puppet=%p clump=%p) -- releasing hold",
                eid, senderSlot, puppet, clump);
        ReleaseClientHold(s, static_cast<coop::element::ElementId>(eid));
        return;
    }

    // The throw must release the puppet's grab, so the clump's re-pile gate (the holder's
    // grabbing_actor being valid) reads not held, or it aborts the re-pile; the native release
    // applies no impulse (the launch is inherited kinematic velocity), but a host-driven clump has
    // none, so the host applies the throw velocity itself; and hit notification and physics go
    // on, so the flying clump generates the ground contact that fires its own re-pile graph, and
    // the existing spawn thunk converts to-pile.
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(puppet, clump);   // clear grabbing_actor and release the physics handle
    ue_wrap::engine::SetActorRootNotifyRigidBodyCollision(clump, true);  // re-pile depends on the contact stream
    ue_wrap::engine::SetActorSimulatePhysics(clump, true);
    ue_wrap::engine::SetActorRootCollisionEnabled(clump, /*QueryAndPhysics=*/3);  // collide + land (don't sink)
    // The native release key is the only release input, and the launch is the body's inherited
    // hand or camera motion at release (a still player drops softly, a flick throws), not a fixed
    // impulse: a constant impulse fired a hard throw on every release and never a drop. The carry
    // drive tracks the hold point's smoothed per-tick velocity (the kinematic analog of the native
    // handle's inherited velocity); that is used, clamped to a brisk human maximum, since a raw
    // teleport delta on a fast flick can spike far past any real throw. The direction comes from
    // the hand motion, not the aim, so a soft drop falls straight down and a forward flick flies
    // forward, the native feel.
    ue_wrap::FVector lin;
    if (mode == coop::net::throw_mode::kHardThrow) {
        // The native hard throw: the launch is the engine's projectile-toss suggestion, which
        // reduces to camera-forward times 15000 over the clump's mass floored at 10, plus the
        // thrower's velocity, with no cap, a deliberate throw. The host holds the real clump, so it
        // reads the true mass; the camera forward is the client's at the press, so the clump flies
        // exactly where it looked; the puppet's velocity stands in for the thrower's locomotion.
        const float mass  = ue_wrap::engine::GetActorRootMass(clump);
        const float denom = (mass > 10.f) ? mass : 10.f;   // FMax(objMass, 10) -- a 0/unresolved mass floors to 10
        const float speed = 15000.f / denom;
        const ue_wrap::FVector pv = ue_wrap::engine::GetActorVelocity(puppet);
        lin = ue_wrap::FVector{ camFwd.X * speed + pv.X, camFwd.Y * speed + pv.Y, camFwd.Z * speed + pv.Z };
    } else {
        // The release: the launch is the puppet's smoothed hand motion (a still hold drops softly,
        // a flick flies), capped to a brisk human maximum so a teleport-delta spike is not a wild
        // throw.
        lin = coop::puppet_carry_drive::HandVelocityForEid(static_cast<coop::element::ElementId>(eid));
        constexpr float kMaxThrowCmS = 650.f;   // ~6.5 m/s -- above this is a teleport-delta artifact, not a human throw
        const float sp2 = lin.X * lin.X + lin.Y * lin.Y + lin.Z * lin.Z;
        if (sp2 > kMaxThrowCmS * kMaxThrowCmS) {
            const float sc = kMaxThrowCmS / std::sqrt(sp2);
            lin.X *= sc; lin.Y *= sc; lin.Z *= sc;
        }
    }
    ue_wrap::engine::SetActorRootPhysicsVelocity(clump, lin, ue_wrap::FVector{0.f, 0.f, 0.f});  // apply AFTER SimulatePhysics(true)
    coop::puppet_carry_drive::NoteThrown(static_cast<coop::element::ElementId>(eid));  // stop hand-drive; stream the flight
    // Close the carry latch and held-by record IMMEDIATELY on throw. The clump is now flying and
    // the holder cannot grab anything else. If the clump lands in a dumpster (or is otherwise
    // destroyed before the re-pile thunk fires), g_carry / g_heldBy are already closed so the
    // player can pick up the next piece of trash without waiting for a land-settle that will
    // never arrive. The clump's flight pose is still streamed until it re-piles.
    g_heldBy.erase(eid);
    coop::trash_channel::CloseCarryForEid(eid);
    UE_LOGI("[THROW-INTENT] SUCCESS eid=%u slot=%u mode=%s clump=%p -- puppet released + physics thrown vel=(%.0f,%.0f,%.0f); "
            "clump flies + self-re-piles (thunk -> ToPile)", eid, senderSlot,
            mode == coop::net::throw_mode::kHardThrow ? "hardThrow(LMB)" : "release(E)", clump, lin.X, lin.Y, lin.Z);
}

void OnGrabHolderLeft(uint8_t senderSlot) {
    for (auto it = g_heldBy.begin(); it != g_heldBy.end(); ) {
        if (it->second == senderSlot) {
            UE_LOGI("[GRAB-INTENT] OnGrabHolderLeft slot=%u -- clearing HELD_BY eid=%u + ForgetEid",
                    senderSlot, it->first);
            ForgetEid(static_cast<coop::element::ElementId>(it->first));  // drop a stranded carry latch/settle
            it = g_heldBy.erase(it);
        } else { ++it; }
    }
}

void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E) {
    const uint32_t eid = static_cast<uint32_t>(E);
    if (g_heldBy.erase(eid))
        UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- clump lost before land; hold cleared (re-grabbable)", eid);
    ForgetEid(E);   // drop a stranded carry latch/settle (idempotent if the land COMMIT already closed it)
    // The trash entity vanished on the host (the clump died with no re-pile). Broadcast a destroy
    // for the eid, so every client retires the now-frozen carry mirror and clears its carry toggle
    // (the destroy receiver retires the mirror and clears the carry). Without it the requester is
    // stuck in throw mode for the dead eid forever, every press denied, and its mirror floats in
    // mid-air.
    coop::net::PropDestroyPayload dp{};
    dp.key.len    = 0;            // eid-only: clients resolve the mirror by host-range eid
    dp.elementId  = eid;
    s.SendPropDestroy(dp);
    UE_LOGI("[GRAB-INTENT] ReleaseClientHold eid=%u -- PropDestroy(eid) broadcast (carry ABORT: clients retire "
            "the frozen mirror + clear the carry toggle)", eid);
}

}  // namespace coop::trash_channel
