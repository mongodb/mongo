---- MODULE MCSyncSourceSelection ----
\* MC harness for SyncSourceSelection.tla — provides StateConstraint to bound
\* the model checker, and a small concrete Server set.
\* See SyncSourceSelection.tla for instructions.

EXTENDS SyncSourceSelection

StateConstraint ==
    Len(history) <= MaxDecisions
=============================================================================
