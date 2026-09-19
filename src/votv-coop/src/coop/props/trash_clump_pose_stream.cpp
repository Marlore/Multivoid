// coop/props/trash_clump_pose_stream.cpp -- see coop/props/trash_clump_pose_stream.h.

#include "coop/props/trash_clump_pose_stream.h"

#include "coop/props/active_drive.h"        // ActiveDrive + BeginLerpToPose / AdvanceLerp (the shared interp)
#include "coop/net/protocol.h"        // TrashClumpPoseSnapshot
#include "coop/net/session.h"         // TakeRemoteTrashCarryBatch
#include "coop/props/trash_channel.h"       // IsInboundStreamCtxFresh / CtxForEid (the stale-pose / pile-jump guard)
#include "coop/props/remote_prop.h"         // ResolveLiveActorByEid (the per-eid carry target)
#include "coop/player/puppet_carry_drive.h" // ClearPuppetCarryDriveForEid
#include "ue_wrap/core/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"       // IsLive
#include "ue_wrap/engine/engine.h"          // GetActorLocation / GetActorRotation / GetActorVelocity
#include "ue_wrap/engine/engine_attach.h"   // SetActorSimulatePhysics / SetActorRootPhysicsVelocity / SetActorRootCollisionEnabled

#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace coop::trash_clump_pose_stream {
namespace {

namespace R  = ue_wrap::reflection;
namespace AD = coop::active_drive;

// One ActiveDrive per host-driven trash clump, keyed by eid (NOT a per-slot array -- N clients can each
// carry a clump at once). GAME-THREAD only (every entry point runs on the net-pump game thread).
std::unordered_map<uint32_t, AD::ActiveDrive> g_carryDrives;

// Track which eids have transitioned to physics mode (after a throw). Once physics is enabled,
// we skip the kinematic drive (which teleports via SetActorLocation) and let physics simulate.
// We also track the target position from the pose stream so we can apply bounded position
// corrections when the error grows too large.
struct PhysicsClump {
    bool physicsEnabled = false;   // true once SetActorSimulatePhysics(true) was called
    ue_wrap::FVector lastTargetPos{};  // last pose stream target position (for correction)
    ue_wrap::FVector lastTargetVel{};  // last velocity from pose stream
};
std::unordered_map<uint32_t, PhysicsClump> g_physicsClumps;  // eid -> physics state

// Throttled apply log: the first few, then every 60th.
uint32_t g_applyCount = 0;

// The drained poses, swapped with the session's buffer each take so neither side allocates in the
// steady state.
std::vector<coop::net::TrashClumpPoseSnapshot> g_batch;

}  // namespace

void TickApplyAndDrive(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("trash_clump_pose_stream::TickApplyAndDrive");
    const uint64_t nowMs = AD::NowMs();

    // 1. Drain every pose received since the last tick, the newest per clump (consume-once; nothing
    //    new leaves the existing drives to AdvanceLerp between packets / freeze on a stop). Only the
    //    HOST populates it -> on a host this returns false and we just advance nothing.
    if (s.TakeRemoteTrashCarryBatch(g_batch)) {
        for (const auto& snap : g_batch) {
            const coop::element::ElementId E = static_cast<coop::element::ElementId>(snap.eid);
            // ctx-gate (requireCurrentGen, like remote_prop's PropPose gate): a carry pose for a generation
            // whose ToClump convert we have NOT adopted yet (ctx != known) is HELD -- applying it would drive
            // the still-PRE-convert PILE rendering to the carry point + fire its grab cue before the re-skin
            // (the pile-jump + double-grab-cue glitch). The reliable convert always lands; the continuous
            // stream supersedes the held pose, so the carry just starts AT the convert.
            if (!coop::trash_channel::IsInboundStreamCtxFresh(snap.eid, snap.ctx, /*requireCurrentGen=*/true))
                continue;
            void* mirror = coop::remote_prop::ResolveLiveActorByEid(E);
            if (!mirror) continue;  // no live mirror for this eid yet (its PropSpawn / convert has not landed)
            
            // Check if this pose has non-zero velocity (throw). If so, enable physics on the mirror
            // so the clump flies/falls realistically instead of floating kinematically.
            const float velMag = snap.linVelX * snap.linVelX + snap.linVelY * snap.linVelY + snap.linVelZ * snap.linVelZ;
            const bool hasVelocity = (velMag > 1.0f);  // threshold to avoid noise
            
            // Debug: log first few poses per eid to see what velocity values arrive
            static uint32_t s_lastLogEid = 0;
            static int s_logCount = 0;
            if (snap.eid != s_lastLogEid) {
                s_lastLogEid = snap.eid;
                s_logCount = 0;
            }
            if (s_logCount < 5) {
                UE_LOGI("[TRASH-CARRY] CLIENT RECEIVED eid=%u pos=(%.1f,%.1f,%.1f) vel=(%.0f,%.0f,%.0f) mag=%.1f ctx=%u",
                        snap.eid, snap.x, snap.y, snap.z,
                        snap.linVelX, snap.linVelY, snap.linVelZ, velMag,
                        static_cast<unsigned>(snap.ctx));
                s_logCount++;
            }
            
            if (hasVelocity) {
                // Transition to physics mode: enable simulation and apply velocity
                // Only do this once per throw (idempotent)
                auto& pc = g_physicsClumps[snap.eid];
                if (!pc.physicsEnabled) {
                    pc.physicsEnabled = true;
                    // Enable physics simulation on the clump mirror
                    bool simOk = ue_wrap::engine::SetActorSimulatePhysics(mirror, true);
                    // Enable collision (QueryAndPhysics=3) so it lands correctly
                    ue_wrap::engine::SetActorRootCollisionEnabled(mirror, 3);
                    // Apply the launch velocity from the host
                    ue_wrap::engine::SetActorRootPhysicsVelocity(
                        mirror, 
                        ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ},
                        ue_wrap::FVector{snap.angVelX, snap.angVelY, snap.angVelZ}
                    );
                    pc.lastTargetVel = ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ};
                    const uint32_t n = ++g_applyCount;
                    if (n <= 3 || (n % 60) == 0)
                        UE_LOGI("[TRASH-CARRY] CLIENT PHYSICS ON eid=%u simOk=%d vel=(%.0f,%.0f,%.0f) -> clump will fly/fall",
                                snap.eid, simOk ? 1 : 0,
                                snap.linVelX, snap.linVelY, snap.linVelZ);
                } else {
                    // Physics already enabled: update target velocity for correction
                    pc.lastTargetVel = ue_wrap::FVector{snap.linVelX, snap.linVelY, snap.linVelZ};
                }
                // Store target position for bounded correction
                pc.lastTargetPos = ue_wrap::FVector{snap.x, snap.y, snap.z};
                // SKIP the kinematic drive below -- physics owns the position now.
                // The ActiveDrive would teleport via SetActorLocation, fighting physics.
                continue;
            }
            
            // No velocity (kinematic carry): use the normal ActiveDrive path.
            AD::ActiveDrive& d = g_carryDrives[snap.eid];
            if (d.actor != mirror || d.lastEid != snap.eid) {   // first pose for this eid, or the mirror actor changed
                AD::ResetDriveState(d);
                d.actor    = mirror;
                d.actorIdx = R::InternalIndexOf(mirror);  // rooted + live here; cache for IsLiveByIndex
                d.isTrashMirror = true;       // host-authoritative follower: freeze on a gap, never drop to physics
                d.lastEid  = snap.eid;
            }
            AD::BeginLerpToPose(d, ue_wrap::FVector{snap.x, snap.y, snap.z},
                                ue_wrap::FRotator{snap.pitch, snap.yaw, snap.roll}, nowMs);
            const uint32_t n = ++g_applyCount;
            if (n <= 3 || (n % 60) == 0)
                UE_LOGI("[TRASH-CARRY] CLIENT APPLY eid=%u ctx=%u -> target(%.1f,%.1f,%.1f) (per-eid carry drive)",
                        snap.eid, static_cast<unsigned>(snap.ctx), snap.x, snap.y, snap.z);
        }
        g_batch.clear();   // capacity kept; the session's buffer must come back empty on the next take
    }

    // 2. Advance EVERY drive one tick (whether or not a new pose arrived): smooth follow between sendHz
    //    poses + freeze at the last target on a stream gap. Prune a drive whose mirror actor died (a retire
    //    that did not route through ClearDriveForEid -- defensive; AdvanceLerp itself no-ops on a dead actor).
    for (auto it = g_carryDrives.begin(); it != g_carryDrives.end(); ) {
        AD::ActiveDrive& d = it->second;
        if (!d.LiveActor()) { it = g_carryDrives.erase(it); continue; }  // slot-validated
        AD::AdvanceLerp(d, nowMs);
        ++it;
    }
    
    // 3. Bounded position correction for physics-mode clumps: if the client's clump drifts
    //    too far from the host's target position, apply a corrective velocity to the physics
    //    body (NOT a teleport, which would fight physics). This keeps sync without breaking
    //    the physics simulation.
    constexpr float kMaxDriftCm = 200.f;  // 2m max drift before correction
    constexpr float kCorrectiveVelScale = 5.f;  // corrective velocity multiplier
    for (auto& [eid, pc] : g_physicsClumps) {
        if (!pc.physicsEnabled) continue;
        void* mirror = coop::remote_prop::ResolveLiveActorByEid(static_cast<coop::element::ElementId>(eid));
        if (!mirror || !R::IsLive(mirror)) continue;
        
        // Calculate drift from target position
        ue_wrap::FVector curPos = ue_wrap::engine::GetActorLocation(mirror);
        float dx = curPos.X - pc.lastTargetPos.X;
        float dy = curPos.Y - pc.lastTargetPos.Y;
        float dz = curPos.Z - pc.lastTargetPos.Z;
        float driftSq = dx * dx + dy * dy + dz * dz;
        
        if (driftSq > kMaxDriftCm * kMaxDriftCm) {
            // Apply corrective velocity towards target (bounded)
            float drift = std::sqrt(driftSq);
            float correction = std::min(drift * kCorrectiveVelScale, 2000.f);  // cap at 20 m/s
            ue_wrap::FVector corrVel{
                (dx / drift) * correction,
                (dy / drift) * correction,
                (dz / drift) * correction
            };
            // Add to current physics velocity (don't overwrite)
            ue_wrap::FVector curVel;
            ue_wrap::engine::GetActorRootPhysicsVelocity(mirror, curVel, curVel);  // reuse as temp
            ue_wrap::FVector newVel{curVel.X + corrVel.X, curVel.Y + corrVel.Y, curVel.Z + corrVel.Z};
            ue_wrap::engine::SetActorRootPhysicsVelocity(
                mirror, newVel, ue_wrap::FVector{0.f, 0.f, 0.f}
            );
            UE_LOGI("[TRASH-CARRY] CLIENT PHYSICS CORRECTION eid=%u drift=%.1fcm -> vel=(%.0f,%.0f,%.0f)",
                    eid, drift, newVel.X, newVel.Y, newVel.Z);
        }
    }
}

void ClearDriveForEid(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_clump_pose_stream::ClearDriveForEid");
    if (g_carryDrives.erase(static_cast<uint32_t>(eid)))
        UE_LOGI("[TRASH-CARRY] CLIENT clear carry drive eid=%u (land / retire)", static_cast<unsigned>(eid));
    // Also clear physics mode flag for this eid
    g_physicsClumps.erase(static_cast<uint32_t>(eid));
    // Also clear the puppet_carry_drive entry (which keeps flying=true after throw)
    coop::puppet_carry_drive::ClearPuppetCarryDriveForEid(eid);
}

void OnDisconnect() {
    g_carryDrives.clear();
    g_batch.clear();
    g_physicsClumps.clear();
}

}  // namespace coop::trash_clump_pose_stream
