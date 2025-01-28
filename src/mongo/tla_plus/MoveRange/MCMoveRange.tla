---------------------------------- MODULE MCMoveRange ----------------------------------------------
\* This module defines MoveRange.tla constants/constraints for model-checking.

EXTENDS MoveRange

(**************************************************************************************************)
(* State Constraints.                                                                             *)
(**************************************************************************************************)

\* Intentionally overlaps with the `PropertyAllKeysReturned' liveness invariant.
ConstraintAllKeysReturned == /\ UNION {sReturned[s] : s \in Shards} # Keys

\* Define symmetry for TLA to avoid exploring equivalent states.
Symmetry == Permutations(Shards) \union Permutations(Keys)

(**************************************************************************************************)
(* Counterexamples.                                                                               *)
(**************************************************************************************************)

\* The ownership filter omits an unowned key from the result set.
BaitOrphanedFilteredOut == \A s \in Shards : sExamined[s] = sReturned[s]

\* A migration starts.
BaitMigrationStarts == /\ migrations = 0

\* A migration commits.
BaitMigrationCommits ==
    /\ IF migrationState = [k \in Keys |-> EmptyMigrationState] \* No ongoing migrations.
        THEN \A r \in Router : rCachedVersions[r] = sVersions   \* Bait: up-to-date router.
        ELSE TRUE

====================================================================================================
