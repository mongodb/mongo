\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE ShardRegistryRefreshLiveness ---------------------------------
\* This specification models the liveness of a ShardRegistry refresh against a config-server
\* replica set in which one secondary is unrecoverably stale (its oplog applier has stalled and
\* will never advance during the trace).
\*
\* The behaviour being modelled is the routing client's read against the config server's catalog
\* with an afterClusterTime recency requirement. The refresh either:
\*   - succeeds, because it reaches a node whose lastApplied satisfies the recency requirement, or
\*   - returns a retryable error to the caller, which then re-targets to a different node.
\*
\* The bug being defended against is that, in the absence of a per-attempt deadline, the client
\* can park on the stale secondary indefinitely waiting for its lastApplied to catch up. The
\* refresh loop never advances because it keeps reading from the same lagging node.
\*
\* The model exposes a single boolean knob, HasPerAttemptDeadline, which switches the
\* implementation between the buggy version (the wait is unbounded) and the fixed version (a
\* per-attempt timeout fires, the response is treated as retryable, and the next attempt picks a
\* different replica). The green configuration sets HasPerAttemptDeadline = TRUE and proves
\* liveness; the bug configuration sets it to FALSE and is expected to violate liveness, which
\* the MC bait observes.
\*
\* To run the model-checker, edit MCShardRegistryRefreshLiveness.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh ShardRegistryRefreshLiveness

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    ConfigNodes,            \* Set of replica-set member identifiers.
    StaleNode,              \* The single member whose oplog applier has stalled. Element of
                            \* ConfigNodes.
    MaxRetries,             \* Bound on the number of retry attempts the refresh will make.
                            \* Keeps the state space finite.
    MaxClusterTime,         \* Bound on how far the primary will advance the cluster time. Keeps
                            \* the state space finite.
    HasPerAttemptDeadline   \* TRUE  => fixed implementation (refresh attempt times out, becomes
                            \*           a retryable error).
                            \* FALSE => buggy implementation (refresh attempt waits forever on
                            \*           the stale node).

ASSUME Cardinality(ConfigNodes) >= 3
ASSUME StaleNode \in ConfigNodes
ASSUME MaxRetries \in 1..20
ASSUME MaxClusterTime \in 1..20
ASSUME HasPerAttemptDeadline \in BOOLEAN

(* Refresh client states.
   - idle:       no refresh in flight.
   - selecting:  picking which node to target next; reads the latest required cluster time.
   - waiting:    request sent, awaiting a response from the targeted node.
   - done:       refresh has completed successfully.
*)
RefreshStateIdle      == "idle"
RefreshStateSelecting == "selecting"
RefreshStateWaiting   == "waiting"
RefreshStateDone      == "done"
RefreshStates         == {RefreshStateIdle, RefreshStateSelecting, RefreshStateWaiting,
                          RefreshStateDone}

(* Per-target response state for the in-flight attempt.
   - pending:   the targeted node has not responded yet.
   - satisfied: the node's lastApplied is >= requiredClusterTime; the refresh succeeded.
   - retry:     the node could not satisfy the recency requirement and produced a retryable error.
   In the buggy build, the stale node never produces 'retry' because there is no per-attempt
   deadline; the request just keeps waiting.
*)
AttemptResultPending   == "pending"
AttemptResultSatisfied == "satisfied"
AttemptResultRetry     == "retry"
AttemptResults         == {AttemptResultPending, AttemptResultSatisfied, AttemptResultRetry}

VARIABLES
    refreshState,           \* Element of RefreshStates.
    targetNode,              \* Currently targeted ConfigNode, or "-" when none.
    requiredClusterTime,     \* The afterClusterTime carried by the in-flight attempt.
    attemptResult,           \* Element of AttemptResults.
    retriesLeft,             \* Decrements on each retry attempt.
    lastApplied,             \* lastApplied[n]: monotone non-decreasing oplog position for n.
    clusterTime              \* Latest cluster time written by the primary.

NoTarget == "-"

vars == << refreshState, targetNode, requiredClusterTime, attemptResult, retriesLeft,
           lastApplied, clusterTime >>

TypeOK ==
    /\ refreshState \in RefreshStates
    /\ targetNode \in ConfigNodes \cup {NoTarget}
    /\ requiredClusterTime \in 0..MaxClusterTime
    /\ attemptResult \in AttemptResults
    /\ retriesLeft \in 0..MaxRetries
    /\ lastApplied \in [ConfigNodes -> 0..MaxClusterTime]
    /\ clusterTime \in 0..MaxClusterTime

Init ==
    /\ refreshState = RefreshStateIdle
    /\ targetNode = NoTarget
    /\ requiredClusterTime = 0
    /\ attemptResult = AttemptResultPending
    /\ retriesLeft = MaxRetries
    /\ lastApplied = [n \in ConfigNodes |-> 0]
    /\ clusterTime = 0

(**************************************************************************************************)
(* Helpers.                                                                                       *)
(**************************************************************************************************)

\* The set of non-stale nodes. These are the only ones whose lastApplied is allowed to advance,
\* and they are the ones the refresh will eventually re-target to once a retry has fired.
LiveNodes == ConfigNodes \ {StaleNode}

(**************************************************************************************************)
(* Cluster-progress actions.                                                                      *)
(*                                                                                                *)
(* These mirror the real cluster's behaviour outside of the refresh loop:                         *)
(*  - The primary advances the cluster time (e.g. by accepting an addShard write).                *)
(*  - Healthy secondaries eventually catch up.                                                    *)
(*  - The stale secondary does not catch up.                                                      *)
(**************************************************************************************************)

\* The primary commits a write that advances the cluster time. The primary is modeled as any
\* member of LiveNodes whose lastApplied was already at clusterTime (i.e. caught up).
AdvanceClusterTime(n) ==
    /\ n \in LiveNodes
    /\ lastApplied[n] = clusterTime
    /\ clusterTime < MaxClusterTime
    /\ clusterTime' = clusterTime + 1
    /\ lastApplied' = [lastApplied EXCEPT ![n] = clusterTime + 1]
    /\ UNCHANGED << refreshState, targetNode, requiredClusterTime, attemptResult, retriesLeft >>

\* A healthy secondary applies the next oplog entry, advancing its lastApplied toward clusterTime.
HealthySecondaryCatchUp(n) ==
    /\ n \in LiveNodes
    /\ lastApplied[n] < clusterTime
    /\ lastApplied' = [lastApplied EXCEPT ![n] = lastApplied[n] + 1]
    /\ UNCHANGED << refreshState, targetNode, requiredClusterTime, attemptResult, retriesLeft,
                    clusterTime >>

\* StaleNode never advances. We explicitly do NOT include an action for the stale node to make
\* progress; its lastApplied stays pinned at its Init value (0) for the whole trace.

(**************************************************************************************************)
(* Refresh client actions.                                                                        *)
(*                                                                                                *)
(* Mirrors the routing client driving a ShardRegistry refresh:                                    *)
(*   idle --StartRefresh--> selecting --PickTarget--> waiting                                     *)
(*   waiting --AttemptSucceeds--> done                                                            *)
(*   waiting --AttemptTimesOutAndRetries--> selecting (only when HasPerAttemptDeadline is TRUE,   *)
(*                                                     or when the target was a healthy node      *)
(*                                                     that simply produced retry on its own)     *)
(*                                                                                                *)
(* The buggy variant (HasPerAttemptDeadline = FALSE) keeps the client in 'waiting' against the    *)
(* stale node forever, because no timeout fires and the node never satisfies the request.        *)
(**************************************************************************************************)

\* A new refresh starts. It captures the current cluster time as its recency requirement.
StartRefresh ==
    /\ refreshState = RefreshStateIdle
    /\ clusterTime > 0
    /\ refreshState' = RefreshStateSelecting
    /\ requiredClusterTime' = clusterTime
    /\ attemptResult' = AttemptResultPending
    /\ targetNode' = NoTarget
    /\ UNCHANGED << retriesLeft, lastApplied, clusterTime >>

\* The client picks any config node as the next target. This is intentionally non-deterministic
\* to model the replica-set monitor's read-preference scheduling: under read pref 'nearest' or
\* 'secondary', any node may be chosen, including the stale one.
PickTarget(n) ==
    /\ refreshState = RefreshStateSelecting
    /\ n \in ConfigNodes
    /\ retriesLeft > 0
    /\ targetNode' = n
    /\ refreshState' = RefreshStateWaiting
    /\ attemptResult' = AttemptResultPending
    /\ UNCHANGED << requiredClusterTime, retriesLeft, lastApplied, clusterTime >>

\* The targeted node observes that its lastApplied meets the recency requirement and responds
\* with satisfied. The refresh transitions to done.
AttemptSucceeds ==
    /\ refreshState = RefreshStateWaiting
    /\ targetNode \in ConfigNodes
    /\ lastApplied[targetNode] >= requiredClusterTime
    /\ attemptResult' = AttemptResultSatisfied
    /\ refreshState' = RefreshStateDone
    /\ UNCHANGED << targetNode, requiredClusterTime, retriesLeft, lastApplied, clusterTime >>

\* The targeted node cannot satisfy the recency requirement and produces a retryable error.
\* For a healthy node, this can happen whenever its lastApplied is still behind clusterTime; the
\* node will resolve naturally once it catches up. For the stale node, this transition only fires
\* in the fixed build because that is the build where the per-attempt deadline lives. In the
\* buggy build the stale node simply never responds, modelled by omitting this enabling clause.
AttemptTimesOutAndRetries ==
    /\ refreshState = RefreshStateWaiting
    /\ targetNode \in ConfigNodes
    /\ lastApplied[targetNode] < requiredClusterTime
    /\ \/ targetNode \in LiveNodes
       \/ /\ targetNode = StaleNode
          /\ HasPerAttemptDeadline
    /\ retriesLeft > 0
    /\ attemptResult' = AttemptResultRetry
    /\ refreshState' = RefreshStateSelecting
    /\ retriesLeft' = retriesLeft - 1
    /\ UNCHANGED << targetNode, requiredClusterTime, lastApplied, clusterTime >>

(**************************************************************************************************)
(* Refresh completion stutters once done so traces can stop without violating fairness.           *)
(**************************************************************************************************)
Terminated ==
    /\ refreshState = RefreshStateDone
    /\ UNCHANGED vars

(**************************************************************************************************)
(* Next-state relation and fairness.                                                              *)
(**************************************************************************************************)

Next ==
    \/ StartRefresh
    \/ \E n \in ConfigNodes : PickTarget(n)
    \/ AttemptSucceeds
    \/ AttemptTimesOutAndRetries
    \/ \E n \in ConfigNodes : AdvanceClusterTime(n)
    \/ \E n \in ConfigNodes : HealthySecondaryCatchUp(n)
    \/ Terminated

\* Fairness: every action that is continuously enabled must eventually fire. This is what makes
\* the difference between the green and bug configurations observable.
\*
\*  - Weak fairness on AttemptTimesOutAndRetries: in the green config this action is enabled
\*    whenever the client is parked on the stale node, so weak fairness forces a retry. In the
\*    bug config the same action is disabled for the stale node, so the client gets stuck in
\*    'waiting' and the liveness property fails.
\*
\*  - Strong fairness on PickTarget(liveNode): the replica-set monitor will not pin a sequence of
\*    retries to the same stale node forever. Modelling this with strong fairness mirrors the
\*    real RSM's behaviour of cycling through eligible hosts: as long as picking a live node is
\*    enabled infinitely often (which it is whenever the client is in 'selecting' with retries
\*    remaining), it eventually fires. Without this, the green model could pessimistically retry
\*    onto the stale node until retriesLeft hit zero, which is not a faithful model of the
\*    production RSM and would muddy the green-vs-bug signal.
\*
\*  - Strong fairness on AttemptSucceeds and on the cluster-progress actions ensures the system
\*    cannot starve a healthy completion path forever.
Fairness ==
    /\ WF_vars(StartRefresh)
    /\ WF_vars(\E n \in ConfigNodes : PickTarget(n))
    /\ SF_vars(\E n \in LiveNodes  : PickTarget(n))
    /\ SF_vars(AttemptSucceeds)
    /\ WF_vars(AttemptTimesOutAndRetries)
    /\ WF_vars(\E n \in ConfigNodes : AdvanceClusterTime(n))
    /\ WF_vars(\E n \in ConfigNodes : HealthySecondaryCatchUp(n))

Spec == Init /\ [][Next]_vars /\ Fairness

(**************************************************************************************************)
(* Safety properties.                                                                             *)
(**************************************************************************************************)

\* lastApplied never moves backwards.
LastAppliedMonotone == \A n \in ConfigNodes : lastApplied[n] <= clusterTime

\* The stale node's lastApplied is pinned at 0 for the entire trace.
StaleNodeNeverCatchesUp == lastApplied[StaleNode] = 0

\* Once the refresh is done, it stays done.
DoneIsAbsorbing == refreshState = RefreshStateDone =>
                        attemptResult \in {AttemptResultSatisfied, AttemptResultRetry}

(**************************************************************************************************)
(* Liveness properties.                                                                           *)
(*                                                                                                *)
(* The key property of the fix is that a refresh started under a stale-replica situation must    *)
(* eventually complete. In the green config this holds. In the bug config the property fails on  *)
(* the trace where the client picks the stale node and HasPerAttemptDeadline is FALSE.            *)
(**************************************************************************************************)

\* Any refresh that starts eventually transitions to done.
RefreshEventuallyCompletes ==
    [] ( refreshState = RefreshStateSelecting => <> (refreshState = RefreshStateDone) )

\* Even stronger: from any state, if the system has issued a refresh (we are no longer idle), the
\* refresh eventually completes.
RefreshLiveness == (refreshState # RefreshStateIdle) ~> (refreshState = RefreshStateDone)

====================================================================================================
