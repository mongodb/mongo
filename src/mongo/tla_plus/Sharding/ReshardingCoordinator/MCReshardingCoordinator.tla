------------------------------ MODULE MCReshardingCoordinator --------------------------------------
\* This module defines ReshardingCoordinator.tla constants/constraints for model-checking.

EXTENDS ReshardingCoordinator

(**************************************************************************************************)
(* Symmetry: donors and recipients are interchangeable within their respective sets.              *)
(**************************************************************************************************)

Symmetry == Permutations(Donors) \union Permutations(Recipients)

(**************************************************************************************************)
(* State constraint: cap on the total transitions to keep the model finite under fairness.        *)
(* (Stepdown count is bounded by MaxStepdowns; this caps committed/aborted churn.)                *)
(**************************************************************************************************)

StateBound == stepdownCount <= MaxStepdowns

(**************************************************************************************************)
(* Bait predicates: useful for sanity-checking the model (TLC will print a trace).                *)
(**************************************************************************************************)

\* Reach the success terminal at least once.
BaitSuccess == ~committed

\* Reach the aborting state at least once.
BaitAborting == coordState # CoordAborting

\* Observe at least one stepdown+stepup cycle.
BaitStepupCycle == stepdownCount = 0

====================================================================================================
