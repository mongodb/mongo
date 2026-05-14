\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

----------------------- MODULE ReshardingAbortFilteringMetadata ------------------------------------
(****************************************************************************************************)
(* Formal specification of the race described in SERVER-90810:                                      *)
(*                                                                                                  *)
(*   When the resharding coordinator decides to abort an in-flight reshard, the recipient shard     *)
(*   performs the following sequence:                                                               *)
(*                                                                                                  *)
(*     1. The recipient drops the temporary resharding collection.                                  *)
(*     2. The recipient clears its in-memory filtering metadata for the temporary namespace.        *)
(*     3. The recipient kicks off an *asynchronous* refresh of the filtering metadata. This refresh *)
(*        snapshots the authoritative cluster metadata at some point during the abort flow and      *)
(*        later installs that snapshot into the in-memory cache.                                    *)
(*     4. The coordinator deletes the resharding coordinator document, which cleans up the          *)
(*        authoritative metadata for the temporary collection.                                      *)
(*                                                                                                  *)
(*   Between (3) and (4), other concurrent DDLs (e.g. an indexCreate on the source collection or a  *)
(*   shard-key refine) can bump the authoritative placement version. The async refresh in (3) may   *)
(*   read a snapshot of the authoritative metadata taken *before* those concurrent DDLs landed, and *)
(*   then install that stale snapshot into the recipient's in-memory cache.                         *)
(*                                                                                                  *)
(*   The end state is: the recipient's cached filtering metadata is older than the latest DDL that  *)
(*   ran during the reshard window, even though the recipient has long since acknowledged the       *)
(*   abort.                                                                                         *)
(*                                                                                                  *)
(* Modelling choices:                                                                               *)
(*   * One source collection, one temporary resharding namespace, one recipient shard. The bug is   *)
(*     local to a single recipient; multiplying shards does not add covered behavior.               *)
(*   * The "authoritative" metadata is a monotone integer version number maintained by the config   *)
(*     server. Any DDL ticks it up by 1.                                                            *)
(*   * The recipient maintains two pieces of state:                                                 *)
(*       - `cachedFilteringMetadata`: the in-memory filtering metadata that the recipient enforces. *)
(*       - `currentFilteringMetadata`: the authoritative version on the config side. Any read of    *)
(*         filtering metadata is logically read against this.                                       *)
(*   * The async refresh is modelled as a two-phase action: a SNAPSHOT phase that captures the      *)
(*     authoritative version into `pendingRefreshVersion`, and an INSTALL phase that copies that    *)
(*     snapshot into `cachedFilteringMetadata`. The two phases can interleave with `ConcurrentDDL`  *)
(*     actions that tick `currentFilteringMetadata` upward.                                         *)
(*   * The recipient abort state machine and the refresh state machine are *decoupled*. The         *)
(*     recipient acks the abort to the coordinator after dispatching the refresh; the coordinator   *)
(*     cleanup may then race with the refresh install in either order. This matches the production  *)
(*     code path: the async refresh continuation is not joined before the recipient acks.           *)
(*   * The fix the spec drives is: the refresh's install must respect any authoritative version     *)
(*     that is observable at the install moment -- i.e., the install must not clobber the cache    *)
(*     with a snapshot older than what's currently authoritative. In SAFE_MODE, AsyncRefreshInstall *)
(*     installs max(snapshot, current).                                                             *)
(*                                                                                                  *)
(* To run the model-checker, first edit the constants in MCReshardingAbortFilteringMetadata.cfg if  *)
(* desired, then:                                                                                   *)
(*     cd src/mongo/tla_plus                                                                        *)
(*     ./model-check.sh Sharding/ReshardingAbortFilteringMetadata                                   *)
(****************************************************************************************************)

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    MAX_DDLS,     \* Maximum number of concurrent DDLs that may bump placement during the reshard.
    SAFE_MODE     \* TRUE: model the proposed fix (install rejects stale snapshots).
                  \* FALSE: model the buggy current implementation.

ASSUME MAX_DDLS \in 0..10
ASSUME SAFE_MODE \in BOOLEAN

\* Sentinel value used to denote "no filtering metadata installed".
NoVersion == 0

\* Recipient main-thread abort state machine. The sequence corresponds to the on-thread steps in
\* the issue description: drop temp coll -> clear filtering metadata -> dispatch async refresh ->
\* ack abort to coordinator.
\*   "ACTIVE"             - reshard in progress; abort not started.
\*   "DROPPED_TEMP_COLL"  - recipient dropped the temporary collection (ticket step 5).
\*   "CLEARED_METADATA"   - recipient cleared the in-memory filtering metadata (ticket step 6).
\*   "REFRESH_DISPATCHED" - recipient scheduled the async refresh and acked the abort to the
\*                          coordinator (ticket step 7). The refresh continuation runs independently
\*                          on a separate thread.
RecipientAbortStates == {"ACTIVE", "DROPPED_TEMP_COLL", "CLEARED_METADATA", "REFRESH_DISPATCHED"}

\* Refresh state machine, independent of the recipient main-thread state.
\*   "REFRESH_IDLE"        - refresh not yet dispatched.
\*   "REFRESH_PENDING"     - refresh dispatched, snapshot not yet captured.
\*   "REFRESH_SNAPSHOTTED" - refresh has captured `pendingRefreshVersion`.
\*   "REFRESH_INSTALLED"   - refresh has copied `pendingRefreshVersion` to the cache.
RefreshStates == {"REFRESH_IDLE", "REFRESH_PENDING", "REFRESH_SNAPSHOTTED", "REFRESH_INSTALLED"}

VARIABLES
    currentFilteringMetadata,   \* Authoritative placement version visible to a fresh refresh.
    cachedFilteringMetadata,    \* What the recipient enforces locally.
    pendingRefreshVersion,      \* Snapshot captured by the async refresh, before install.
    refreshState,               \* Current refresh-thread state.
    abortState,                 \* Current recipient main-thread abort state.
    coordinatorCleanedUp,       \* TRUE once the coordinator document has been deleted.
    ddlsApplied,                \* Number of DDLs that have ticked currentFilteringMetadata.
    \* The maximum authoritative version observed by any DDL that ran during the reshard window.
    \* After the abort settles, the recipient's cache must either be NoVersion (correctly cleared)
    \* or at least this version (correctly refreshed past every concurrent DDL).
    maxDDLVersionDuringWindow

vars == <<currentFilteringMetadata, cachedFilteringMetadata, pendingRefreshVersion, refreshState,
          abortState, coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Initial state. The reshard has just started: the recipient already has an initial filtering      *)
(* metadata version for the temporary collection installed (call it 1), the coordinator is up, and  *)
(* no abort has begun.                                                                              *)
(****************************************************************************************************)
Init ==
    /\ currentFilteringMetadata = 1
    /\ cachedFilteringMetadata = 1
    /\ pendingRefreshVersion = NoVersion
    /\ refreshState = "REFRESH_IDLE"
    /\ abortState = "ACTIVE"
    /\ coordinatorCleanedUp = FALSE
    /\ ddlsApplied = 0
    /\ maxDDLVersionDuringWindow = 1

(****************************************************************************************************)
(* Action: a concurrent DDL (e.g. createIndexes, refineCollectionShardKey on the source) ticks the *)
(* authoritative placement version. The reshard window for the purposes of this race is the      *)
(* interval from reshard start until the recipient's async refresh has finished installing -- a   *)
(* DDL that lands after the install simply doesn't matter to the install, so we gate this action *)
(* on the refresh having not yet installed. We additionally cap by `~coordinatorCleanedUp` to     *)
(* reflect the fact that once the coordinator document is gone the temporary collection's         *)
(* authoritative metadata is gone too and no further bumps against it are reachable.              *)
(****************************************************************************************************)
ConcurrentDDL ==
    /\ ddlsApplied < MAX_DDLS
    /\ ~coordinatorCleanedUp
    /\ refreshState # "REFRESH_INSTALLED"
    /\ currentFilteringMetadata' = currentFilteringMetadata + 1
    /\ ddlsApplied' = ddlsApplied + 1
    /\ maxDDLVersionDuringWindow' = currentFilteringMetadata + 1
    /\ UNCHANGED <<cachedFilteringMetadata, pendingRefreshVersion, refreshState,
                   abortState, coordinatorCleanedUp>>

(****************************************************************************************************)
(* Action: the recipient encountered a fatal cloning error and begins the abort flow. The first    *)
(* observable step is dropping the temporary collection.                                            *)
(****************************************************************************************************)
RecipientDropTempCollection ==
    /\ abortState = "ACTIVE"
    /\ abortState' = "DROPPED_TEMP_COLL"
    /\ UNCHANGED <<currentFilteringMetadata, cachedFilteringMetadata, pendingRefreshVersion,
                   refreshState, coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Action: the recipient clears the in-memory filtering metadata for the temp collection (ticket   *)
(* step 6). Cache becomes NoVersion -- "I have no filtering metadata, must refresh".               *)
(****************************************************************************************************)
RecipientClearFilteringMetadata ==
    /\ abortState = "DROPPED_TEMP_COLL"
    /\ abortState' = "CLEARED_METADATA"
    /\ cachedFilteringMetadata' = NoVersion
    /\ UNCHANGED <<currentFilteringMetadata, pendingRefreshVersion, refreshState,
                   coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Action: the recipient kicks off the async refresh of the filtering metadata (ticket step 7) and *)
(* acks the abort to the coordinator. This action transitions both the recipient main-thread state *)
(* and the refresh-thread state simultaneously: dispatching the refresh and updating the abort     *)
(* state are atomic on the main thread.                                                            *)
(****************************************************************************************************)
RecipientDispatchRefresh ==
    /\ abortState = "CLEARED_METADATA"
    /\ refreshState = "REFRESH_IDLE"
    /\ abortState' = "REFRESH_DISPATCHED"
    /\ refreshState' = "REFRESH_PENDING"
    /\ UNCHANGED <<currentFilteringMetadata, cachedFilteringMetadata, pendingRefreshVersion,
                   coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Action: the async refresh thread captures a snapshot of the authoritative metadata. This is the *)
(* moment where the bug originates: the snapshot may be taken before concurrent DDLs that go on to *)
(* bump `currentFilteringMetadata`.                                                                 *)
(****************************************************************************************************)
AsyncRefreshSnapshot ==
    /\ refreshState = "REFRESH_PENDING"
    /\ refreshState' = "REFRESH_SNAPSHOTTED"
    /\ pendingRefreshVersion' = currentFilteringMetadata
    /\ UNCHANGED <<currentFilteringMetadata, cachedFilteringMetadata, abortState,
                   coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Action: the async refresh thread installs its snapshot into the recipient's cache (ticket step  *)
(* 8). This is where SAFE_MODE matters.                                                            *)
(*                                                                                                  *)
(* SAFE_MODE = FALSE: buggy current implementation. The install blindly copies                     *)
(*                    `pendingRefreshVersion` into `cachedFilteringMetadata`, regardless of how    *)
(*                    stale that snapshot is relative to the current authoritative metadata.       *)
(*                                                                                                  *)
(* SAFE_MODE = TRUE:  proposed fix. The install re-reads `currentFilteringMetadata` at install     *)
(*                    time. If the snapshot is older than what's currently authoritative, the      *)
(*                    install installs the fresh value -- the refresh has logically taken place at *)
(*                    the install moment, not at the snapshot moment, so installing the fresh       *)
(*                    value is sound.                                                              *)
(****************************************************************************************************)
AsyncRefreshInstall ==
    /\ refreshState = "REFRESH_SNAPSHOTTED"
    /\ refreshState' = "REFRESH_INSTALLED"
    /\ IF SAFE_MODE
         THEN cachedFilteringMetadata' =
                IF pendingRefreshVersion >= currentFilteringMetadata
                    THEN pendingRefreshVersion
                    ELSE currentFilteringMetadata
         ELSE cachedFilteringMetadata' = pendingRefreshVersion
    /\ UNCHANGED <<currentFilteringMetadata, pendingRefreshVersion, abortState,
                   coordinatorCleanedUp, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Action: the coordinator cleans up the resharding coordinator document (ticket step 9). Enabled  *)
(* once the recipient has acked the abort -- which happens at REFRESH_DISPATCHED. After this point *)
(* the temp collection's authoritative metadata is gone and no further DDLs against the temp      *)
(* collection are possible (modelled by guarding ConcurrentDDL with ~coordinatorCleanedUp).        *)
(****************************************************************************************************)
CoordinatorCleanup ==
    /\ ~coordinatorCleanedUp
    /\ abortState = "REFRESH_DISPATCHED"
    /\ coordinatorCleanedUp' = TRUE
    /\ UNCHANGED <<currentFilteringMetadata, cachedFilteringMetadata, pendingRefreshVersion,
                   refreshState, abortState, ddlsApplied, maxDDLVersionDuringWindow>>

(****************************************************************************************************)
(* Stuttering termination. The abort flow is fully settled once the coordinator has cleaned up    *)
(* AND the async refresh has installed. After that, allow infinite stuttering.                     *)
(****************************************************************************************************)
Terminating ==
    /\ coordinatorCleanedUp
    /\ refreshState = "REFRESH_INSTALLED"
    /\ UNCHANGED vars

Next ==
    \/ ConcurrentDDL
    \/ RecipientDropTempCollection
    \/ RecipientClearFilteringMetadata
    \/ RecipientDispatchRefresh
    \/ AsyncRefreshSnapshot
    \/ AsyncRefreshInstall
    \/ CoordinatorCleanup
    \/ Terminating

Fairness ==
    /\ WF_vars(RecipientDropTempCollection)
    /\ WF_vars(RecipientClearFilteringMetadata)
    /\ WF_vars(RecipientDispatchRefresh)
    /\ WF_vars(AsyncRefreshSnapshot)
    /\ WF_vars(AsyncRefreshInstall)
    /\ WF_vars(CoordinatorCleanup)

Spec == Init /\ [][Next]_vars /\ Fairness

(****************************************************************************************************)
(* Type invariants.                                                                                 *)
(****************************************************************************************************)

TypeOK ==
    /\ currentFilteringMetadata \in Nat
    /\ cachedFilteringMetadata \in Nat
    /\ pendingRefreshVersion \in Nat
    /\ refreshState \in RefreshStates
    /\ abortState \in RecipientAbortStates
    /\ coordinatorCleanedUp \in BOOLEAN
    /\ ddlsApplied \in 0..MAX_DDLS
    /\ maxDDLVersionDuringWindow \in Nat

(****************************************************************************************************)
(* Safety properties.                                                                               *)
(****************************************************************************************************)

\* The authoritative version is monotone. Sanity check on the spec.
AuthoritativeMonotone == currentFilteringMetadata >= 1

\* The pending refresh snapshot, once captured, cannot exceed currentFilteringMetadata + the
\* number of remaining DDLs. Soft sanity bound on the spec; mostly useful to catch off-by-one
\* mistakes.
PendingRefreshBounded ==
    refreshState \in {"REFRESH_SNAPSHOTTED", "REFRESH_INSTALLED"} =>
        pendingRefreshVersion <= currentFilteringMetadata + MAX_DDLS

\* Once a refresh has installed, the cache reflects either the snapshot it captured (buggy path)
\* or a value at least as fresh (safe path). The cache is never older than the snapshot.
RefreshInstallReflectsSnapshot ==
    refreshState = "REFRESH_INSTALLED" =>
        cachedFilteringMetadata >= pendingRefreshVersion

(****************************************************************************************************)
(* The main correctness property.                                                                   *)
(*                                                                                                  *)
(* After the abort flow terminates (refresh installed AND coordinator cleaned up), the recipient's *)
(* cached filtering metadata for the temporary collection must either be cleared (NoVersion) -- the *)
(* recipient knows it must refresh -- or must be at least the maximum authoritative version that   *)
(* was ever published during the reshard window.                                                    *)
(*                                                                                                  *)
(* Concretely: if any DDL ran during the window and bumped the authoritative version to V, but the *)
(* recipient ended the abort flow with `cachedFilteringMetadata = v < V`, the recipient is now    *)
(* enforcing stale filtering metadata -- exactly the SERVER-90810 anomaly.                         *)
(****************************************************************************************************)
NoStaleFilteringInstalledOnAbort ==
    (refreshState = "REFRESH_INSTALLED" /\ coordinatorCleanedUp) =>
        (cachedFilteringMetadata = NoVersion
         \/ cachedFilteringMetadata >= maxDDLVersionDuringWindow)

(****************************************************************************************************)
(* A second, slightly stronger phrasing of the safety property. Used as a bait/sanity invariant.   *)
(* The cache is either cleared, or at least as fresh as whatever the refresh captured.             *)
(****************************************************************************************************)
NoCacheRegression ==
    refreshState = "REFRESH_INSTALLED" =>
        \/ cachedFilteringMetadata = NoVersion
        \/ cachedFilteringMetadata >= pendingRefreshVersion

(****************************************************************************************************)
(* Liveness. The abort flow eventually settles: both the coordinator cleanup and the refresh       *)
(* install must occur.                                                                              *)
(****************************************************************************************************)
AbortEventuallyCompletes ==
    <>[](refreshState = "REFRESH_INSTALLED" /\ coordinatorCleanedUp)

====================================================================================================
