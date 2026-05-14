------------------------- MODULE MCCrossDBRenameWriterFence ---------------------------------------
\* Model-checking module for CrossDBRenameWriterFence.
\*
\* Two .cfg files share this module:
\*   - MCCrossDBRenameWriterFence.cfg         (BUG schedule: HoldSourceLockUntilRename = FALSE)
\*   - MCCrossDBRenameWriterFence_green.cfg   (FIX schedule: HoldSourceLockUntilRename = TRUE)
\*
\* The bug cfg expects NoWriteLost / NoWriteLostAtEnd to fail with a counterexample showing a write
\* acked by the source between the unlocked-P1 and P3-drop window, which then vanishes when source
\* is dropped. The green cfg expects the same invariants to hold across the full state space.

EXTENDS CrossDBRenameWriterFence

(* ----- Symmetry --------------------------------------------------------------------------------- *)

\* All writes are interchangeable from the lost-write perspective; permuting them shouldn't grow
\* the state space.
Symmetry == Permutations(Writes)

(* ----- Bait predicates (counterexample search) -------------------------------------------------- *)

\* A write reaches the source collection while the rename is mid-flight.
BaitWriteRacedRename ==
    \A w \in Writes :
        writerPhase[w] # "acked_source" \/ renamePhase \in {RenameIdle, RenameDone}

\* A write was acked but is nowhere readable after the rename completes.
BaitLostWrite ==
    renamePhase # RenameDone \/
    \A w \in clientAcked : w \in targetDocs \/ w \in sourceDocs

====================================================================================================
