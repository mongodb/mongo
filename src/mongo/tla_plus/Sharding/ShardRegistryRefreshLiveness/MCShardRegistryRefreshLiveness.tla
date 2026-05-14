\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------------- MODULE MCShardRegistryRefreshLiveness ---------------------------------
\* This module is the model-checking entry point for ShardRegistryRefreshLiveness.tla. It binds
\* the model's CONSTANTS and exposes a small handful of bait predicates that are convenient for
\* eyeballing counter-example traces.
\*
\* Two .cfg files live in this directory:
\*   - MCShardRegistryRefreshLiveness.cfg      (green: HasPerAttemptDeadline = TRUE, liveness must
\*                                              hold). This is the file consumed by
\*                                              ../../model-check.sh.
\*   - MCShardRegistryRefreshLiveness_bug.cfg  (bug:  HasPerAttemptDeadline = FALSE, liveness must
\*                                              fail). Run by hand with the .cfg argument flipped:
\*       cd src/mongo/tla_plus/Sharding/ShardRegistryRefreshLiveness
\*       java -cp ../../../tla2tools.jar tlc2.TLC -lncheck final \
\*           -config MCShardRegistryRefreshLiveness_bug.cfg \
\*           MCShardRegistryRefreshLiveness.tla

EXTENDS ShardRegistryRefreshLiveness

(**************************************************************************************************)
(* Bait invariants. Enable in the .cfg to confirm the model can reach the named state.            *)
(**************************************************************************************************)

\* The refresh ever parks on the stale node. If this invariant is enabled and TLC reports no
\* violation, the model is failing to cover the scenario being defended against.
BaitClientTargetsStaleNode ==
    /\ ~ ( refreshState = RefreshStateWaiting /\ targetNode = StaleNode )

\* The refresh ever completes. Useful for confirming the green configuration produces happy
\* terminating traces, not just ones that loop forever.
BaitRefreshCompletes ==
    /\ refreshState # RefreshStateDone

\* A retry ever fires. Without this, the green configuration could be passing trivially because
\* the model never even reached the stale node.
BaitRetryFires ==
    /\ ~ ( refreshState = RefreshStateSelecting /\ attemptResult = AttemptResultRetry )

====================================================================================================
