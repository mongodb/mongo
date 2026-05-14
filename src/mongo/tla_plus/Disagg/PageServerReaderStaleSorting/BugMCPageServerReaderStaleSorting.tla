---- MODULE BugMCPageServerReaderStaleSorting ----
\* Model-checking wrapper that reproduces the bug. The sorting rule is set to
\* PreferLocalUnconditional (mirroring page_server_reader.cpp:707 where an
\* unknown remote frontier is encoded as Timestamp::max()). TLC is expected
\* to produce a counter-example to NoServingStaleReadFromLocal showing the
\* post-step-up sort selecting LocalCell while remote frontiers are unknown
\* and local cannot serve the requested LSN.
\* See PageServerReaderStaleSorting.tla for instructions.

EXTENDS PageServerReaderStaleSorting

CONSTANT MaxSteps

StateConstraint ==
    /\ committedLSN <= MaxLSN
    /\ Len(pickHistory) <= MaxSteps

=============================================================================
