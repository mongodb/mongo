
\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

----------------------- MODULE MCReshardingOplogFetchedAccounting ----------------------------------

EXTENDS ReshardingOplogFetchedAccounting

(***************************************************************************************************)
(* Counterexamples / Baits                                                                         *)
(*                                                                                                 *)
(* Selectively enable one of the following BAIT invariants in the .cfg file to generate a counter- *)
(* example trace illustrating that particular scenario.                                            *)
(***************************************************************************************************)

\* Bait: the resharding op makes forward progress in nominal executions (used to verify the spec
\* itself is not vacuous under the FIX configuration).
BaitCritSecEngages ==
    coordinatorState # "CRIT_SEC_ENGAGED"

\* Bait: the buggy ordering eventually overcounts. With IncrementBeforeInsert = TRUE and at least
\* one InsertFail / RetryAfterFail, this safety violation is reachable — counter exceeds buffer.
BaitOvercountObserved ==
    oplogEntriesFetched <= bufferCount + fetcherBatchSize

\* Bait: the buggy ordering produces a state in which all donor ops are pulled, all buffered ops
\* are drained, yet the fetched/applied ratio remains stuck above the threshold and the
\* coordinator is still in APPLYING. That is, structurally, the SERVER-118706 "hang".
BaitHangReachable ==
    \neg HangScenarioReachable

(***************************************************************************************************)
(* Symmetry — none required: DonorOps and Shards are not modelled as symmetric sets.               *)
(***************************************************************************************************)

====================================================================================================
