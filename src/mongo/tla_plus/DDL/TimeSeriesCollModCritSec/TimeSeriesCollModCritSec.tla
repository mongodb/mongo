\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE TimeSeriesCollModCritSec -----------------------------------------
\* This specification models the sharded time-series collMod coordinator phase machine and the
\* recoverable critical section lifecycle on participant shards. It targets SERVER-125921.
\*
\* The bug: CollModCoordinator engages participant critical sections in kBlockShards by sending
\* _shardsvrParticipantBlock with kReadsAndWrites, and only releases them as a side effect of
\* _shardsvrCollModParticipant in kUpdateShards (when needsUnblock=true). If a non-retriable error
\* fires after kBlockShards but before all participants have released their critical section, the
\* coordinator aborts: it calls resumeMigrations, but it never sends an unblock command. Result:
\* CRUD stays blocked on the still-prepared participants even after the coordinator returns.
\*
\* Phase transitions modelled:
\*   kUnset -> kFreezeMigrations -> kBlockShards -> kUpdateConfig -> kUpdateShards -> Done
\*                                       \         \                       |
\*                                        \         +---(non-retriable)----+--> Aborted
\*                                         +---(non-retriable)-------+--> Aborted
\*
\* The buggy spec lets Aborted leave shard critical sections held. The fixed spec routes through
\* a kReleaseCritSec phase, modelling the fix proposed in the ticket (dedicated release phase
\* and/or a collMod-specific _cleanupOnAbort that issues kUnblock to participants).
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh DDL/TimeSeriesCollModCritSec

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Shards,              \* set of participant shards owning chunks
    MAX_ABORTS,          \* bound non-retriable error injections (state space)
    FIX_ENABLED          \* TRUE  -> coordinator routes abort through kReleaseCritSec
                         \* FALSE -> reproduce buggy behaviour (no release on abort)

ASSUME Cardinality(Shards) > 0
ASSUME MAX_ABORTS \in 0..3
ASSUME FIX_ENABLED \in BOOLEAN

(* Coordinator state *)
VARIABLE phase           \* coordinator phase, one of the Phase* constants below
VARIABLE migrationsFrozen \* models the stopMigrations / resumeMigrations side effect
VARIABLE aborts          \* count of injected non-retriable errors (for state-space bound)

(* Per-participant state *)
VARIABLE sCritSec        \* sCritSec[s] \in {"none", "blocked"} - kReadsAndWrites on the shard
VARIABLE sCollModApplied \* sCollModApplied[s] \in BOOLEAN - whether the shard's local collMod ran

vars == <<phase, migrationsFrozen, aborts, sCritSec, sCollModApplied>>

(* Phases *)
PhaseUnset           == "kUnset"
PhaseFreezeMig       == "kFreezeMigrations"
PhaseBlockShards     == "kBlockShards"
PhaseUpdateConfig    == "kUpdateConfig"
PhaseUpdateShards    == "kUpdateShards"
PhaseReleaseCritSec  == "kReleaseCritSec"   \* present only in the fixed model
PhaseDone            == "kDone"
PhaseAborted         == "kAborted"

Phases == {PhaseUnset, PhaseFreezeMig, PhaseBlockShards, PhaseUpdateConfig,
           PhaseUpdateShards, PhaseReleaseCritSec, PhaseDone, PhaseAborted}

Init ==
    /\ phase = PhaseUnset
    /\ migrationsFrozen = FALSE
    /\ aborts = 0
    /\ sCritSec = [s \in Shards |-> "none"]
    /\ sCollModApplied = [s \in Shards |-> FALSE]

(* Coordinator transitions *)

\* kUnset -> kFreezeMigrations: coordinator persists FreezeMigrations and calls stopMigrations.
EnterFreezeMigrations ==
    /\ phase = PhaseUnset
    /\ phase' = PhaseFreezeMig
    /\ migrationsFrozen' = TRUE
    /\ UNCHANGED <<aborts, sCritSec, sCollModApplied>>

\* kFreezeMigrations -> kBlockShards: coordinator sends _shardsvrParticipantBlock kReadsAndWrites to
\* every participant owning chunks. Modelled as a single-step write to every shard's critical
\* section, matching CollModCoordinator::_runImpl Phase::kBlockShards.
EnterBlockShards ==
    /\ phase = PhaseFreezeMig
    /\ phase' = PhaseBlockShards
    /\ sCritSec' = [s \in Shards |-> "blocked"]
    /\ UNCHANGED <<migrationsFrozen, aborts, sCollModApplied>>

\* kBlockShards -> kUpdateConfig: commit timeseries options to the global / shard catalog.
EnterUpdateConfig ==
    /\ phase = PhaseBlockShards
    /\ phase' = PhaseUpdateConfig
    /\ UNCHANGED <<migrationsFrozen, aborts, sCritSec, sCollModApplied>>

\* kUpdateConfig -> kUpdateShards: send _shardsvrCollModParticipant with needsUnblock=true to each
\* participant. Each participant applies its local collMod, then calls
\* ShardingRecoveryService::releaseRecoverableCriticalSection() (modelled by ApplyCollModOnShard).
EnterUpdateShards ==
    /\ phase = PhaseUpdateConfig
    /\ phase' = PhaseUpdateShards
    /\ UNCHANGED <<migrationsFrozen, aborts, sCritSec, sCollModApplied>>

\* While in kUpdateShards, each shard independently applies the participant command, which also
\* releases its critical section (the needsUnblock side effect).
ApplyCollModOnShard(s) ==
    /\ phase = PhaseUpdateShards
    /\ sCollModApplied[s] = FALSE
    /\ sCollModApplied' = [sCollModApplied EXCEPT ![s] = TRUE]
    /\ sCritSec' = [sCritSec EXCEPT ![s] = "none"]
    /\ UNCHANGED <<phase, migrationsFrozen, aborts>>

\* kUpdateShards -> kDone: all participants have applied and unblocked. Coordinator calls
\* resumeMigrations and returns success.
FinishUpdateShards ==
    /\ phase = PhaseUpdateShards
    /\ \A s \in Shards : sCollModApplied[s] = TRUE
    /\ phase' = PhaseDone
    /\ migrationsFrozen' = FALSE
    /\ UNCHANGED <<aborts, sCritSec, sCollModApplied>>

(* Non-retriable abort injection.
   Models any path that throws a non-retriable DBException after kBlockShards has entered the
   critical section: e.g. shard catalog commit failure, participant collMod failure surfaced as
   IndexNotFound, an interrupted-but-not-retriable error in _sendCollModToParticipantShards, etc.
   The buggy behaviour: resumeMigrations is invoked, but no unblock command is sent. *)
NonRetriableAbort ==
    /\ aborts < MAX_ABORTS
    /\ phase \in {PhaseBlockShards, PhaseUpdateConfig, PhaseUpdateShards}
    /\ aborts' = aborts + 1
    /\ migrationsFrozen' = FALSE   \* matches resumeMigrations in the catch
    /\ IF FIX_ENABLED
         THEN phase' = PhaseReleaseCritSec   \* fix: route through a dedicated release phase
         ELSE phase' = PhaseAborted          \* buggy: jump straight to terminal Aborted
    /\ UNCHANGED <<sCritSec, sCollModApplied>>

\* Fixed model: in kReleaseCritSec, the coordinator sends an explicit _shardsvrParticipantBlock
\* kUnblock (or equivalent _shardsvrCollModParticipant with needsUnblock=true and a no-op spec)
\* to every shard, then transitions to Aborted.
ReleaseCritSecOnShard(s) ==
    /\ FIX_ENABLED
    /\ phase = PhaseReleaseCritSec
    /\ sCritSec[s] = "blocked"
    /\ sCritSec' = [sCritSec EXCEPT ![s] = "none"]
    /\ UNCHANGED <<phase, migrationsFrozen, aborts, sCollModApplied>>

FinishReleaseCritSec ==
    /\ FIX_ENABLED
    /\ phase = PhaseReleaseCritSec
    /\ \A s \in Shards : sCritSec[s] = "none"
    /\ phase' = PhaseAborted
    /\ UNCHANGED <<migrationsFrozen, aborts, sCritSec, sCollModApplied>>

Next ==
    \/ EnterFreezeMigrations
    \/ EnterBlockShards
    \/ EnterUpdateConfig
    \/ EnterUpdateShards
    \/ \E s \in Shards : ApplyCollModOnShard(s)
    \/ FinishUpdateShards
    \/ NonRetriableAbort
    \/ \E s \in Shards : ReleaseCritSecOnShard(s)
    \/ FinishReleaseCritSec

Fairness ==
    /\ WF_vars(EnterFreezeMigrations)
    /\ WF_vars(EnterBlockShards)
    /\ WF_vars(EnterUpdateConfig)
    /\ WF_vars(EnterUpdateShards)
    /\ \A s \in Shards : WF_vars(ApplyCollModOnShard(s))
    /\ WF_vars(FinishUpdateShards)
    /\ \A s \in Shards : WF_vars(ReleaseCritSecOnShard(s))
    /\ WF_vars(FinishReleaseCritSec)

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------------
(*  Type invariants                                                                              *)

TypeOK ==
    /\ phase \in Phases
    /\ migrationsFrozen \in BOOLEAN
    /\ aborts \in 0..MAX_ABORTS
    /\ \A s \in Shards : sCritSec[s] \in {"none", "blocked"}
    /\ \A s \in Shards : sCollModApplied[s] \in BOOLEAN

(*  Correctness properties                                                                       *)

\* The headline safety property: if the coordinator has reached a terminal state, no participant
\* shard is still holding a critical section. This is the invariant SERVER-125921 violates when
\* FIX_ENABLED = FALSE.
NoStrayCriticalSectionOnTermination ==
    (phase \in {PhaseDone, PhaseAborted}) =>
        (\A s \in Shards : sCritSec[s] = "none")

\* Critical sections may only be held while the coordinator is actively in or past kBlockShards
\* and has not yet terminated.
CritSecImpliesActivePhase ==
    \A s \in Shards :
        sCritSec[s] = "blocked" =>
            phase \in {PhaseBlockShards, PhaseUpdateConfig, PhaseUpdateShards,
                       PhaseReleaseCritSec}

\* Migrations must be unfrozen by the time the coordinator terminates (this part already holds in
\* the buggy implementation; pinning it here prevents regressions in the fix).
MigrationsResumedOnTermination ==
    (phase \in {PhaseDone, PhaseAborted}) => (migrationsFrozen = FALSE)

(*  Liveness                                                                                     *)

\* Eventually the coordinator reaches a terminal phase with no critical section held.
EventuallyClean ==
    <>((phase \in {PhaseDone, PhaseAborted}) /\ (\A s \in Shards : sCritSec[s] = "none"))

====================================================================================================
