---------------------------- MODULE MCMigrationConcurrentIndexOps ----------------------------------
\* Model-checking harness for MigrationConcurrentIndexOps.tla. Defines constants, symmetry, and a
\* state constraint that keeps the explored state space finite.

EXTENDS MigrationConcurrentIndexOps

(**************************************************************************************************)
(* State constraints.                                                                              *)
(**************************************************************************************************)

\* Bound the state space: stop exploring after MIGRATIONS migrations and INDEX_OPS index ops have
\* completed and the cluster has quiesced. Without this, TLC can chase identical-shape suffix
\* traces forever once the bug is reachable.
ConstraintBounded ==
    /\ \/ migrationsDone < MIGRATIONS
       \/ indexOpsDone < INDEX_OPS
       \/ ~NoMigrationInFlight
       \/ ~NoIndexOpInFlight

\* Symmetry over indistinguishable shards and index names speeds up checking by orders of
\* magnitude on the small models that exercise this race.
Symmetry == Permutations(Shards) \union Permutations(IndexNames)

(**************************************************************************************************)
(* Counterexample bait predicates (negations of "interesting" states). Useful when iterating on   *)
(* the spec: temporarily flip one into an INVARIANT in the .cfg to make TLC dump a witness trace. *)
(**************************************************************************************************)

\* A migration has committed at least once.
BaitMigrationCommitted == migrationsDone = 0

\* An index op completed while a migration was in flight.
BaitIndexOpDuringMigration ==
    \/ indexOpsDone = 0
    \/ NoMigrationInFlight

====================================================================================================
