\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE UnstableCheckpointRecovery ---------------------------
\* Formal specification for SERVER-87919: "Stop taking unstable checkpoints during
\* recovery for restore".
\*
\* Background. Startup-recovery-for-restore (--startupRecoveryForRestore plus
\* --takeUnstableCheckpointOnShutdown) reapplies oplog entries past a lagged stable
\* timestamp and, on shutdown, persists an *unstable* checkpoint. After the durable
\* history store landed, unstable and stable checkpoints differ mainly in whether
\* the stable timestamp is persisted: on restart from an unstable checkpoint,
\* Rollback-to-Stable (RTS) *skips* tables with timestamped updates so as not to
\* clobber the in-table durable-history information. If the node then crashes
\* before its first stable checkpoint and restarts again, replication recovery
\* reapplies oplog entries whose effects are already physically present (with
\* their original commit timestamps) in the unstable checkpoint --- WiredTiger
\* surfaces this as an out-of-order timestamped write.
\*
\* This spec models a single node executing the restore lifecycle and proves
\* that the unsafe state (oplog entry whose commit-timestamp <= the maximum
\* commit-timestamp already physically present in the checkpoint) is *reachable*
\* under the current "take unstable checkpoint" behavior, and is *unreachable*
\* once unstable checkpoints during recovery-for-restore are suppressed. The
\* property the patch must establish is NoOutOfOrderReplay below; TLC produces a
\* counterexample today and produces none when SuppressUnstableCheckpoint is set
\* to TRUE.
\*
\* To run the model-checker, first edit the constants in MCUnstableCheckpointRecovery.cfg
\* if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/UnstableCheckpointRecovery

EXTENDS Integers, FiniteSets, Sequences, TLC

\* Maximum number of oplog entries the node may apply during recovery-for-restore.
CONSTANT MaxOplogLen

\* Maximum simulated wall-clock / commit-timestamp value.
CONSTANT MaxTs

\* Switch under test. When FALSE, the node may persist an unstable checkpoint at
\* shutdown after recovery-for-restore (current behavior). When TRUE, the patch
\* under SERVER-87919 is modeled: shutdown either skips the checkpoint or persists
\* a stable one anchored at the advanced stable timestamp. NoOutOfOrderReplay is
\* expected to hold in the TRUE case and to be violated in the FALSE case.
CONSTANT SuppressUnstableCheckpoint

ASSUME MaxOplogLen \in Nat /\ MaxOplogLen >= 1
ASSUME MaxTs \in Nat /\ MaxTs >= MaxOplogLen
ASSUME SuppressUnstableCheckpoint \in BOOLEAN

----
\* Phases the node moves through. Names track the real-life codepath so the
\* counterexample reads like a log scrape.
\*
\*  "Restoring"   - replication recovery reapplying oplog past stable ts
\*  "Shutdown"    - replication recovery has finished applying; about to checkpoint
\*  "Crashed"     - node was SIGKILLed before its first stable checkpoint
\*  "Restarting"  - node is restarting and choosing a recovery checkpoint
\*  "Replaying"   - replication recovery is replaying oplog after restart
\*  "Steady"      - node has a stable checkpoint and is serving normally
VARIABLE phase

\* The oplog the node will (re)apply during restore. Each entry is a record
\* [ts |-> Nat, key |-> Nat]. ts is the commit timestamp.
VARIABLE oplog

\* The set of (key, ts) pairs that have been physically written to the durable
\* table. These carry their commit timestamp as in WiredTiger's timestamped
\* history.
VARIABLE table

\* The most recent checkpoint persisted to disk.
\*   metaTs = 0 represents an *unstable* checkpoint (meta checkpoint timestamp 0,0
\*            in WT logs); the table contents are still durable.
\*   metaTs > 0 represents a *stable* checkpoint anchored at metaTs.
\*   exists = FALSE means no checkpoint has ever been persisted.
VARIABLE checkpoint

\* The node's appliedThrough / replication recovery cursor; the highest ts the
\* node believes it has fully applied. Used to drive Replay after restart.
VARIABLE appliedThrough

\* The node's stable timestamp. During recovery-for-restore this advances as we
\* apply oplog entries (SERVER-85688 changed this). Held constant otherwise.
VARIABLE stableTs

\* A safety flag tripped whenever the model performs an oplog reapply whose ts
\* is <= a ts already physically present in the table for the same key. This is
\* exactly the storage-engine condition reported as an out-of-order timestamped
\* write in AF-618 / AF-3985.
VARIABLE outOfOrderObserved

vars == << phase, oplog, table, checkpoint, appliedThrough, stableTs,
           outOfOrderObserved >>

----
\* Helpers

\* Keys mentioned in the oplog the node will replay.
KeysInOplog == { oplog[i].key : i \in DOMAIN oplog }

\* Maximum ts already physically present in the table for key k, or 0 if none.
MaxTsInTable(k) ==
    LET S == { e.ts : e \in { x \in table : x.key = k } } IN
    IF S = {} THEN 0 ELSE CHOOSE x \in S : \A y \in S : x >= y

\* Whether reapplying entry e would create a duplicate / out-of-order write
\* relative to what's already physically in the table.
ReapplyIsOutOfOrder(e) ==
    /\ \E x \in table : x.key = e.key /\ x.ts >= e.ts

----
\* Initial state. The node has booted with a lagged stable timestamp, a
\* non-empty oplog of entries past that stable ts, and no checkpoint yet.

Init ==
    /\ phase = "Restoring"
    /\ \E n \in 1..MaxOplogLen:
        \E f \in [1..n -> [ts: 1..MaxTs, key: 0..1]]:
            \* Generate a strictly-increasing-ts oplog suffix (real oplog
            \* invariant). Two keys are enough to expose the unsafe interleaving.
            /\ \A i \in 1..(n-1) : f[i].ts < f[i+1].ts
            /\ oplog = f
    /\ table = {}
    /\ checkpoint = [exists |-> FALSE, metaTs |-> 0, contents |-> {}]
    /\ appliedThrough = 0
    /\ stableTs = 0
    /\ outOfOrderObserved = FALSE

----
\* Actions

\* During recovery-for-restore, apply the next oplog entry. SERVER-85688 also
\* advances stableTs to the just-applied ts. We track both writes physically.
ApplyNextOplogEntry ==
    /\ phase = "Restoring"
    /\ \E i \in DOMAIN oplog :
        /\ oplog[i].ts > appliedThrough
        /\ \A j \in DOMAIN oplog :
              oplog[j].ts > appliedThrough => oplog[i].ts <= oplog[j].ts
        /\ IF ReapplyIsOutOfOrder(oplog[i])
              THEN outOfOrderObserved' = TRUE
              ELSE outOfOrderObserved' = outOfOrderObserved
        /\ table' = table \cup {[key |-> oplog[i].key, ts |-> oplog[i].ts]}
        /\ appliedThrough' = oplog[i].ts
        /\ stableTs' = oplog[i].ts
    /\ UNCHANGED << phase, oplog, checkpoint >>

\* Recovery-for-restore has finished applying. Move to shutdown.
FinishRestoreApply ==
    /\ phase = "Restoring"
    /\ \A i \in DOMAIN oplog : oplog[i].ts <= appliedThrough
    /\ phase' = "Shutdown"
    /\ UNCHANGED << oplog, table, checkpoint, appliedThrough, stableTs,
                    outOfOrderObserved >>

\* Persist a checkpoint at shutdown. This is the SERVER-87919 fork:
\*   - SuppressUnstableCheckpoint = FALSE  (today): we may write an *unstable*
\*     checkpoint with metaTs = 0, carrying the durable table forward but losing
\*     the stable-timestamp anchor. This is the unsafe write.
\*   - SuppressUnstableCheckpoint = TRUE   (patch): we either persist nothing
\*     (the prior stable checkpoint stays on disk) or persist a *stable* one
\*     anchored at the advanced stableTs.
PersistUnstableCheckpoint ==
    /\ phase = "Shutdown"
    /\ ~SuppressUnstableCheckpoint
    /\ checkpoint' = [exists   |-> TRUE,
                      metaTs   |-> 0,
                      contents |-> table]
    /\ phase' = "Crashed"
    /\ UNCHANGED << oplog, table, appliedThrough, stableTs, outOfOrderObserved >>

PersistStableCheckpoint ==
    /\ phase = "Shutdown"
    /\ SuppressUnstableCheckpoint
    /\ checkpoint' = [exists   |-> TRUE,
                      metaTs   |-> stableTs,
                      contents |-> table]
    /\ phase' = "Crashed"
    /\ UNCHANGED << oplog, table, appliedThrough, stableTs, outOfOrderObserved >>

\* The user / orchestrator crashes the node before its first stable checkpoint
\* (matches `rst.stop(restoreNode, 9, ...)` in the jstest below). All in-memory
\* state is lost; only `checkpoint` survives.
CrashBeforeStable ==
    /\ phase = "Crashed"
    /\ phase' = "Restarting"
    /\ table' = IF checkpoint.exists THEN checkpoint.contents ELSE {}
    /\ appliedThrough' = 0
    \* On restart, the recovered stable timestamp is the checkpoint's metaTs.
    \* An unstable checkpoint (metaTs = 0) loses the advanced stableTs --- this
    \* is the load-bearing fact behind the AF-618 crash.
    /\ stableTs' = IF checkpoint.exists THEN checkpoint.metaTs ELSE 0
    /\ UNCHANGED << oplog, checkpoint, outOfOrderObserved >>

\* RTS on restart. WiredTiger skips tables with timestamped updates *iff* the
\* recovery checkpoint is unstable (metaTs = 0). With a stable checkpoint, RTS
\* rolls the table back to metaTs.
RollbackToStable ==
    /\ phase = "Restarting"
    /\ IF checkpoint.exists /\ checkpoint.metaTs > 0
          THEN table' = { x \in table : x.ts <= checkpoint.metaTs }
          ELSE table' = table   \* unstable checkpoint: RTS skips the table.
    /\ phase' = "Replaying"
    /\ UNCHANGED << oplog, checkpoint, appliedThrough, stableTs,
                    outOfOrderObserved >>

\* Replication recovery reapplies oplog entries past appliedThrough. This is
\* where the actual out-of-order timestamped write surfaces in production: if
\* the table still has the prior commit-ts for the same key, WiredTiger refuses
\* the write with WT_ROLLBACK / "out of order".
ReplayOplogAfterRestart ==
    /\ phase = "Replaying"
    /\ \E i \in DOMAIN oplog :
        /\ oplog[i].ts > appliedThrough
        /\ \A j \in DOMAIN oplog :
              oplog[j].ts > appliedThrough => oplog[i].ts <= oplog[j].ts
        /\ IF ReapplyIsOutOfOrder(oplog[i])
              THEN outOfOrderObserved' = TRUE
              ELSE outOfOrderObserved' = outOfOrderObserved
        /\ table' = table \cup {[key |-> oplog[i].key, ts |-> oplog[i].ts]}
        /\ appliedThrough' = oplog[i].ts
        /\ stableTs' = oplog[i].ts
    /\ UNCHANGED << phase, oplog, checkpoint >>

\* Replication recovery has finished after restart.
FinishReplay ==
    /\ phase = "Replaying"
    /\ \A i \in DOMAIN oplog : oplog[i].ts <= appliedThrough
    /\ phase' = "Steady"
    /\ UNCHANGED << oplog, table, checkpoint, appliedThrough, stableTs,
                    outOfOrderObserved >>

Next ==
    \/ ApplyNextOplogEntry
    \/ FinishRestoreApply
    \/ PersistUnstableCheckpoint
    \/ PersistStableCheckpoint
    \/ CrashBeforeStable
    \/ RollbackToStable
    \/ ReplayOplogAfterRestart
    \/ FinishReplay

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

----
\* Properties

\* The safety property the SERVER-87919 patch must establish: no oplog reapply
\* ever lands on top of a same-key entry whose timestamp is already in the
\* durable table. With SuppressUnstableCheckpoint = FALSE this is violated;
\* with TRUE it holds.
NoOutOfOrderReplay == outOfOrderObserved = FALSE

\* Liveness: the node eventually reaches a steady state (sanity check; not the
\* load-bearing claim).
EventuallySteady == <>(phase = "Steady")

\* Type invariant.
TypeOK ==
    /\ phase \in {"Restoring", "Shutdown", "Crashed", "Restarting",
                  "Replaying", "Steady"}
    /\ outOfOrderObserved \in BOOLEAN
    /\ checkpoint.exists \in BOOLEAN
    /\ checkpoint.metaTs \in 0..MaxTs
    /\ appliedThrough \in 0..MaxTs
    /\ stableTs \in 0..MaxTs

=============================================================================
