\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------- MODULE MCReshardingAbortFilteringMetadata -----------------------------------
\* Model-checking harness for ReshardingAbortFilteringMetadata.
\*
\* Two configurations live alongside this module:
\*
\*   * MCReshardingAbortFilteringMetadata.cfg
\*     Green configuration. SAFE_MODE = TRUE. The proposed fix installs max(snapshot, current)
\*     instead of blindly clobbering the cache with the captured snapshot. All invariants pass.
\*
\*   * MCReshardingAbortFilteringMetadata_bug.cfg
\*     Bug-reproducing configuration. SAFE_MODE = FALSE. TLC produces a counterexample of the
\*     SERVER-90810 race: a concurrent DDL ticks the authoritative version after the async refresh
\*     captures its snapshot but before the install lands, the install writes the stale snapshot,
\*     and `NoStaleFilteringInstalledOnAbort` is violated.

EXTENDS ReshardingAbortFilteringMetadata

(****************************************************************************************************)
(* State constraints.                                                                               *)
(****************************************************************************************************)

\* The state space is finite by construction (MAX_DDLS caps DDLs, the abort flow is monotone), so a
\* state constraint is not strictly required. We still cap the explored versions defensively to
\* keep counterexample traces compact.
StateConstraint ==
    /\ currentFilteringMetadata <= MAX_DDLS + 2
    /\ ddlsApplied <= MAX_DDLS

(****************************************************************************************************)
(* Counterexample bait properties. Each, when used as an INVARIANT, generates a counterexample      *)
(* trace exhibiting the named scenario. Off by default; flip the desired bait on in the cfg.       *)
(****************************************************************************************************)

\* Bait: an abort flow that completes without any concurrent DDL having run. Useful smoke test that
\* the spec actually reaches the terminal state.
BaitCleanAbort ==
    ~ (refreshState = "REFRESH_INSTALLED" /\ coordinatorCleanedUp /\ ddlsApplied = 0)

\* Bait: an abort flow in which at least one concurrent DDL ran. This is the precondition for the
\* stale-install bug, and the bait confirms the spec can produce such interleavings.
BaitDDLDuringAbort ==
    ~ (refreshState = "REFRESH_INSTALLED" /\ coordinatorCleanedUp /\ ddlsApplied >= 1)

\* Bait: the install lands before the coordinator has cleaned up. This is the moment the bug
\* opens its window. The bait confirms the interleaving is reachable.
BaitInstallBeforeCleanup ==
    ~ (refreshState = "REFRESH_INSTALLED" /\ ~coordinatorCleanedUp)

\* Bait: the cache ends up at a value strictly older than the maximum authoritative version that
\* was published during the reshard window. When SAFE_MODE = FALSE this is reachable; when
\* SAFE_MODE = TRUE this is never reachable.
BaitStaleCacheReachable ==
    ~ (refreshState = "REFRESH_INSTALLED" /\ coordinatorCleanedUp
       /\ ddlsApplied >= 1
       /\ cachedFilteringMetadata < maxDDLVersionDuringWindow
       /\ cachedFilteringMetadata > 0)

====================================================================================================
