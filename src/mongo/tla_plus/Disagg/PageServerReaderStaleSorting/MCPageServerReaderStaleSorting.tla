---- MODULE MCPageServerReaderStaleSorting ----
\* Model-checking wrapper for PageServerReaderStaleSorting.tla with the FIX
\* sorting rule (PreferKnownOverUnknown). The safety invariant
\* NoServingStaleReadFromLocal must hold.
\* See PageServerReaderStaleSorting.tla for instructions.

EXTENDS PageServerReaderStaleSorting

CONSTANT MaxSteps

\* Bound the search space so model-checking terminates: cap committed-LSN
\* advances and stop once the trace contains MaxSteps step-up picks.
StateConstraint ==
    /\ committedLSN <= MaxLSN
    /\ Len(pickHistory) <= MaxSteps

=============================================================================
