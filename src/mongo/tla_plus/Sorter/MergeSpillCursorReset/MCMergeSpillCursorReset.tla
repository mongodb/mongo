---- MODULE MCMergeSpillCursorReset ----
\* Model-checking constants for MergeSpillCursorReset.tla.
\* See MergeSpillCursorReset.tla for instructions.
\*
\* InputKeys is declared here (not in the .cfg) because TLC's config
\* parser does not accept sequence-of-sequences literals. NumSpills,
\* MaxWCE, MaxBatch and ResetAffectsReadCursors stay in the .cfg so the
\* "green" and "bug" runs can flip the boolean without editing this file.

EXTENDS MergeSpillCursorReset

\* Two sorted input spills, jointly covering {1, 2, 3, 4, 5} once each.
\* Two spills × at most 5 keys keeps the reachable state space small
\* enough for a thorough exhaustive check (< 30k states under MaxWCE=2,
\* MaxBatch=2) while still exercising every interleaving of:
\*    - reads from either spill becoming the merge head
\*    - a WCE firing mid-batch with the buffer half-filled
\*    - a WCE firing with the merge iterator already past one spill
\*    - back-to-back WCEs on the same retry attempt
ModelInputKeys == << <<1, 3, 5>>, <<2, 4>> >>

\* State-space bound: cap the number of WCE rollbacks per behaviour at
\* MaxWCE. The total number of state-machine steps is also implicitly
\* bounded by the finite ModelInputKeys.
StateConstraint ==
    /\ WCECount <= MaxWCE

================================================================================
