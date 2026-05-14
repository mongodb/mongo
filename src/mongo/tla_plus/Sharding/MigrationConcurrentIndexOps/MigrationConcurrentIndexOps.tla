\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE MigrationConcurrentIndexOps -------------------------------------
\* This specification models the race between chunk migration and the user-issued index commands
\* (createIndexes and dropIndexes) on a sharded collection. The race is documented in SERVER-99357:
\* a dropIndexes or createIndexes issued through the router executes via a shard-version retry loop
\* that visits each data-bearing shard sequentially. The migration recipient clones the index
\* catalog from the donor at the start of the clone phase and does not re-fetch it on commit.
\* Therefore, an index op that interleaves between
\*   (a) the recipient's index-clone snapshot, and
\*   (b) the router's per-shard fan-out reaching the donor
\* can leave the cluster with different index sets on different shards after the migration commits,
\* which then prevents future migrations (the recipient destination manager refuses to migrate into
\* a shard that has documents but a different index set) and blocks removeShard /
\* transitionToDedicatedConfigServer.
\*
\* This specification:
\* - Models a single chunk-bearing collection. The collection lives on a Donor shard and a
\*   pre-existing Recipient shard (which may or may not own a chunk for the collection).
\*   Additional shards are modelled as bystanders so that the router fan-out has more than one
\*   destination and the ordering of per-shard execution matters.
\* - Models the migration lifecycle as the four phases that matter for index synchronisation:
\*       Unset -> Cloning -> RecipientPrepared -> AllPrepared -> Committed -> Unset
\*   The Aborted branch is folded back into Unset. Only Cloning -> RecipientPrepared is the window
\*   in which the recipient takes its index snapshot.
\* - Models the index commands as router-driven fan-outs that visit each data-bearing shard
\*   sequentially in a non-deterministic order, mirroring the production retry loop. Each shard
\*   acknowledges by mutating its local index set.
\* - The "bug toggle" SafeIndexSyncOnCommit, when TRUE, re-clones the donor's index catalog on
\*   commit (the fix). When FALSE (default), the recipient keeps its stale clone-time snapshot
\*   (the bug). The invariant IndexSetConsistentPostMigration is expected to hold under TRUE and
\*   to be violated by a counter-example trace under FALSE.
\*
\* To run the model-checker, edit MCMigrationConcurrentIndexOps.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/MigrationConcurrentIndexOps

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Shards,             \* The set of data-bearing shards. Cardinality >= 2.
    IndexNames,         \* The set of user-index names (excluding the implicit _id index).
    MIGRATIONS,         \* Bound on the number of migrations explored.
    INDEX_OPS,          \* Bound on the number of createIndexes / dropIndexes operations explored.
    SafeIndexSyncOnCommit  \* Bug toggle. TRUE = fix, FALSE = bug.

ASSUME Cardinality(Shards) >= 2
ASSUME Cardinality(IndexNames) >= 1
ASSUME MIGRATIONS \in 0..4
ASSUME INDEX_OPS \in 0..4
ASSUME SafeIndexSyncOnCommit \in BOOLEAN

(* Migration-phase tokens. *)
PhaseUnset       == "unset"
PhaseCloning     == "cloning"
PhaseRecipPrep   == "recipientPrepared"
PhaseAllPrep     == "allPrepared"
PhaseCommitted   == "committed"

(* Migration-state tokens. *)
EmptyMigration == [phase     |-> PhaseUnset,
                   donor     |-> "-",
                   recipient |-> "-",
                   snapshot  |-> {}]      \* Index-name snapshot the recipient cloned from donor.

(* Index-op kind. *)
OpCreate == "createIndexes"
OpDrop   == "dropIndexes"

VARIABLES
    sIndexes,           \* Per-shard local index set: [Shards -> SUBSET IndexNames].
    sOwns,              \* Per-shard ownership of the (single) collection: [Shards -> BOOLEAN].
                        \* A shard "owns" the collection iff it has at least one chunk and is
                        \* therefore data-bearing for index sync purposes.
    migration,          \* Current migration state (single in-flight migration at a time).
    migrationsDone,     \* Counter, bounded by MIGRATIONS.
    indexOp,            \* In-flight index op:
                        \*   [kind, name, pending: Seq(Shards), done: SUBSET Shards]
                        \* or the sentinel NoIndexOp.
    indexOpsDone        \* Counter, bounded by INDEX_OPS.

NoIndexOp == [kind |-> "none", name |-> "-", pending |-> <<>>, done |-> {}]

vars == <<sIndexes, sOwns, migration, migrationsDone, indexOp, indexOpsDone>>

(* Helpers. *)
DataBearingShards == {s \in Shards : sOwns[s]}
NoMigrationInFlight == migration.phase = PhaseUnset
NoIndexOpInFlight == indexOp.kind = "none"
MigrationParticipants == IF migration.phase = PhaseUnset
                        THEN {}
                        ELSE {migration.donor, migration.recipient}

\* The "fan-out target set" for a router-driven index op: all currently data-bearing shards. A
\* migration recipient that does not yet own a chunk is NOT in this set, modelling the production
\* behaviour where the router uses its cached routing table at command start.
IndexOpTargets == DataBearingShards

\* True iff a shard is currently a migration participant in a phase where it has either acquired
\* the critical section or is about to apply the index snapshot. Index-op steps against such a
\* shard must wait under the fix; under the bug the step proceeds and the recipient's snapshot
\* drifts out of sync.
IsIndexOpBlockedByMigration(s) ==
    /\ ~NoMigrationInFlight
    /\ migration.phase \in {PhaseCloning, PhaseRecipPrep, PhaseAllPrep, PhaseCommitted}
    /\ s \in MigrationParticipants

\* The set of shards a router-driven index op must visit. The router enumerates shards from the
\* routing table at command start; it does NOT re-read the table between per-shard hops. We model
\* this by freezing the fan-out at op start (see StartIndexOp).
RECURSIVE SeqToSet(_)
SeqToSet(s) == IF Len(s) = 0 THEN {} ELSE {s[1]} \cup SeqToSet(Tail(s))

\* True iff the index op visits a participant of the in-flight migration.
OpTouchesMigration ==
    /\ ~NoIndexOpInFlight
    /\ ~NoMigrationInFlight
    /\ (SeqToSet(indexOp.pending) \cup indexOp.done) \cap MigrationParticipants # {}

Init ==
    /\ sOwns \in [Shards -> BOOLEAN]
       \* At least one shard owns the collection. The non-owners are bystander shards.
    /\ \E s \in Shards : sOwns[s] = TRUE
    /\ sIndexes = [s \in Shards |-> {}]
    /\ migration = EmptyMigration
    /\ migrationsDone = 0
    /\ indexOp = NoIndexOp
    /\ indexOpsDone = 0

(*************************************************************************************************)
(* Index-op actions: createIndexes / dropIndexes.                                                *)
(*                                                                                               *)
(* A router-driven index command:                                                                *)
(*   1. Enumerates data-bearing shards from the cached routing table (StartIndexOp).             *)
(*   2. Issues the command shard-by-shard in some order (StepIndexOp). The shard mutates its     *)
(*      local index set on receipt.                                                              *)
(*   3. Completes when all shards have acknowledged (FinishIndexOp).                             *)
(*                                                                                               *)
(* Production has a shard-version retry loop; the spec abstracts that away: the per-shard step   *)
(* is unconditional and idempotent. What matters for the race is the ordering between the        *)
(* per-shard step and the migration's clone-time snapshot.                                       *)
(*************************************************************************************************)

StartIndexOp(kind, n) ==
    /\ NoIndexOpInFlight
    /\ indexOpsDone < INDEX_OPS
    /\ n \in IndexNames
    /\ kind \in {OpCreate, OpDrop}
       \* Under the fix, the index op takes a DDL lock that conflicts with an in-flight migration.
       \* Under the bug, no such serialisation: the index op can race with the migration's
       \* clone-time snapshot.
    /\ IF SafeIndexSyncOnCommit
        THEN NoMigrationInFlight
        ELSE TRUE
    /\ LET targets == DataBearingShards IN
        \E ordering \in [1..Cardinality(targets) -> targets] :
            /\ {ordering[i] : i \in DOMAIN ordering} = targets
            /\ indexOp' = [kind |-> kind,
                           name |-> n,
                           pending |-> [i \in 1..Cardinality(targets) |-> ordering[i]],
                           done |-> {}]
    /\ UNCHANGED <<sIndexes, sOwns, migration, migrationsDone, indexOpsDone>>

StepIndexOp ==
    /\ ~NoIndexOpInFlight
    /\ Len(indexOp.pending) > 0
    /\ LET s == indexOp.pending[1] IN
        /\ IF indexOp.kind = OpCreate
            THEN sIndexes' = [sIndexes EXCEPT ![s] = @ \cup {indexOp.name}]
            ELSE sIndexes' = [sIndexes EXCEPT ![s] = @ \ {indexOp.name}]
        /\ indexOp' = [indexOp EXCEPT !.pending = Tail(@),
                                       !.done = @ \cup {s}]
    /\ UNCHANGED <<sOwns, migration, migrationsDone, indexOpsDone>>

FinishIndexOp ==
    /\ ~NoIndexOpInFlight
    /\ Len(indexOp.pending) = 0
    /\ indexOp' = NoIndexOp
    /\ indexOpsDone' = indexOpsDone + 1
    /\ UNCHANGED <<sIndexes, sOwns, migration, migrationsDone>>

(*************************************************************************************************)
(* Migration actions: clone -> recipientPrepared -> allPrepared -> commit -> unset.              *)
(*************************************************************************************************)

\* Recipient takes the donor's current index snapshot. THIS IS THE BUG SURFACE: the snapshot is
\* never refreshed if the donor's index set mutates after this point.
MigrateStartClone(from, to) ==
    /\ NoMigrationInFlight
    /\ migrationsDone < MIGRATIONS
    /\ from \in Shards /\ to \in Shards
    /\ from # to
    /\ sOwns[from] = TRUE
       \* Either the recipient already owns a chunk (additive migration) or doesn't. Both happen
       \* in production; both must satisfy the invariant.
       \* Under the fix, the migration takes a DDL lock that conflicts with an in-flight index op.
       \* Under the bug, no such serialisation.
    /\ IF SafeIndexSyncOnCommit
        THEN NoIndexOpInFlight
        ELSE TRUE
    /\ migration' = [phase |-> PhaseCloning,
                     donor |-> from,
                     recipient |-> to,
                     snapshot |-> sIndexes[from]]
    /\ UNCHANGED <<sIndexes, sOwns, migrationsDone, indexOp, indexOpsDone>>

\* Recipient applies the cloned index set to its local catalog. In production the recipient
\* createIndexes the missing indexes and (if strict-sync) drops indexes not in the snapshot.
\* Under the fix we model strict sync: the recipient's index set is REPLACED by the snapshot,
\* which mirrors `_dropLocalIndexes` followed by createIndexes on the missing specs (see
\* migration_destination_manager.cpp `_cloneCollectionIndexesAndOptions`). Under the bug we model
\* the historical non-strict / auto-heal path which UNIONS the snapshot into the recipient's
\* existing set, leaving any pre-existing recipient-only indexes behind to diverge from the
\* donor.
MigrateRecipientApplyClone ==
    /\ migration.phase = PhaseCloning
    /\ IF SafeIndexSyncOnCommit
        THEN sIndexes' = [sIndexes EXCEPT ![migration.recipient] = migration.snapshot]
        ELSE sIndexes' = [sIndexes EXCEPT ![migration.recipient] = @ \cup migration.snapshot]
    /\ migration' = [migration EXCEPT !.phase = PhaseRecipPrep]
    /\ UNCHANGED <<sOwns, migrationsDone, indexOp, indexOpsDone>>

MigrateDonorPrepare ==
    /\ migration.phase = PhaseRecipPrep
    /\ migration' = [migration EXCEPT !.phase = PhaseAllPrep]
    /\ UNCHANGED <<sIndexes, sOwns, migrationsDone, indexOp, indexOpsDone>>

\* Commit on the config shard. The recipient gains ownership; the donor retains ownership.
\* (We over-approximate by leaving the donor as data-bearing, which is the harder-to-satisfy case
\* for index consistency.) Under the fix, no in-flight index op can exist here (StartIndexOp
\* takes the DDL lock that conflicts with the migration), so the recipient's clone-time snapshot
\* equals the donor's current index set and the commit is a no-op for index sync.
MigrateCommit ==
    /\ migration.phase = PhaseAllPrep
    /\ LET to == migration.recipient IN
        /\ sOwns' = [sOwns EXCEPT ![to] = TRUE]
    /\ migration' = [migration EXCEPT !.phase = PhaseCommitted]
    /\ UNCHANGED <<sIndexes, migrationsDone, indexOp, indexOpsDone>>

MigrateFinish ==
    /\ migration.phase = PhaseCommitted
    /\ migration' = EmptyMigration
    /\ migrationsDone' = migrationsDone + 1
    /\ UNCHANGED <<sIndexes, sOwns, indexOp, indexOpsDone>>

\* Abort an in-flight migration before commit. Returns the recipient's index catalog to whatever
\* it was prior to the clone application (modelled by leaving it as-is, since auto-heal-added
\* indexes do not get rolled back in production either -- that orphan-index state is one of the
\* failure modes the bug introduces).
MigrateAbort ==
    /\ migration.phase \in {PhaseCloning, PhaseRecipPrep, PhaseAllPrep}
    /\ migration' = EmptyMigration
    /\ UNCHANGED <<sIndexes, sOwns, migrationsDone, indexOp, indexOpsDone>>

(*************************************************************************************************)
(* Next-state relation.                                                                          *)
(*************************************************************************************************)

Next ==
    \/ \E kind \in {OpCreate, OpDrop}, n \in IndexNames : StartIndexOp(kind, n)
    \/ StepIndexOp
    \/ FinishIndexOp
    \/ \E from, to \in Shards : MigrateStartClone(from, to)
    \/ MigrateRecipientApplyClone
    \/ MigrateDonorPrepare
    \/ MigrateCommit
    \/ MigrateFinish
    \/ MigrateAbort

Fairness ==
    /\ WF_vars(StepIndexOp)
    /\ WF_vars(FinishIndexOp)
    /\ WF_vars(MigrateRecipientApplyClone)
    /\ WF_vars(MigrateDonorPrepare)
    /\ WF_vars(MigrateCommit)
    /\ WF_vars(MigrateFinish)

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants.                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ sIndexes \in [Shards -> SUBSET IndexNames]
    /\ sOwns \in [Shards -> BOOLEAN]
    /\ migration.phase \in {PhaseUnset, PhaseCloning, PhaseRecipPrep, PhaseAllPrep, PhaseCommitted}
    /\ migrationsDone \in 0..MIGRATIONS
    /\ indexOpsDone \in 0..INDEX_OPS

(**************************************************************************************************)
(* Correctness Properties.                                                                         *)
(**************************************************************************************************)

\* The headline invariant. After every migration commits and no index op is in flight, all
\* data-bearing shards agree on the index set. This is exactly the property SERVER-99357
\* documents as violated.
IndexSetConsistentPostMigration ==
    \/ ~NoMigrationInFlight                  \* Migration in flight: divergence permitted.
    \/ ~NoIndexOpInFlight                    \* Index op in flight: per-shard transient drift OK.
    \/ \A s1, s2 \in DataBearingShards : sIndexes[s1] = sIndexes[s2]

\* A weaker shape used as a sanity invariant: the migration phase is always well-formed.
MigrationPhaseWellFormed ==
    \/ migration.phase = PhaseUnset
    \/ /\ migration.donor \in Shards
       /\ migration.recipient \in Shards
       /\ migration.donor # migration.recipient

\* Liveness: every started migration eventually clears, and every started index op eventually
\* finishes. (Disabled in the .cfg by default to keep model-checking fast; enable to discharge.)
EventuallyQuiesces == <>(NoMigrationInFlight /\ NoIndexOpInFlight)
====================================================================================================
