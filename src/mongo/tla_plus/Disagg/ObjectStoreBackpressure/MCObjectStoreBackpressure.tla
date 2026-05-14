---------------------------- MODULE MCObjectStoreBackpressure ----------------------------
\* Model-checking entry point for ObjectStoreBackpressure.tla.
\* This module defines constants/symmetry/state-constraints for TLC.
\*
\* To run the healthy configuration:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/ObjectStoreBackpressure
\*
\* The bug configuration (BudgetsEnabled = FALSE) lives in
\* MCObjectStoreBackpressureBug.cfg next to this file. It is intentionally
\* expected to FAIL FairnessAcrossTenants; the failure trace is the
\* counter-example SLS-4797 calls for. Run it manually with:
\*     java ... tlc2.TLC -config MCObjectStoreBackpressureBug.cfg \
\*         MCObjectStoreBackpressure.tla

EXTENDS ObjectStoreBackpressure

(**************************************************************************************************)
(* Symmetry reductions.                                                                           *)
(**************************************************************************************************)

\* Tenants and operation-kinds are symmetric: the spec treats them uniformly
\* up to the per-bucket budget. Permuting their identifiers yields a
\* behaviour-equivalent state, so TLC can collapse them.
\*
\* Note: TLC warns when SYMMETRY is combined with liveness checking, since
\* fairness obligations are not always preserved under permutation. We use
\* symmetry only in the healthy cfg, where the spec is expected to PASS;
\* the bug cfg drops symmetry so its counter-example is unambiguous.
Symmetry == Permutations(Tenants) \union Permutations(OpKinds)

(**************************************************************************************************)
(* State constraints.                                                                             *)
(**************************************************************************************************)

\* Bound the state space so TLC terminates. ShedCount and RetryCount are
\* monotone observability counters; capping them at MaxPendingPerBucket
\* keeps the state space finite without affecting any enabling condition
\* (they appear only on the right-hand side of action updates).
CounterCeiling ==
    /\ \A t \in Tenants, k \in OpKinds : shedCount[t][k] <= MaxPendingPerBucket
    /\ \A t \in Tenants, k \in OpKinds : retryCount[t][k] <= MaxPendingPerBucket

(**************************************************************************************************)
(* Counter-example baits (model-checking aids).                                                   *)
(**************************************************************************************************)

\* Bait: confirm at least one tenant can progress under the chosen cfg.
\* Setting this as an INVARIANT yields a trace where a tenant completes a
\* request, useful for sanity-checking that the spec is exercised.
BaitSomeTenantProgressed == \A t \in Tenants : ~progress[t]

\* Bait: confirm the global cap is fully saturated at some point.
BaitGlobalSaturated == TotalInflight < GlobalCap

\* Bait: confirm the shed path can fire under the healthy cfg (budget shed
\* requires BudgetsEnabled = TRUE and a bucket already at TenantCap).
BaitShedFires == \A t \in Tenants, k \in OpKinds : shedCount[t][k] = 0

============================================================================================
