------------------------------ MODULE MCPSRShutdownLiveness ---------------------------------
\* Model-checking harness for PSRShutdownLiveness.
\*
\* Two cfg files share this module:
\*   MCPSRShutdownLiveness.cfg            -- EnableDrain = TRUE, all invariants must hold.
\*   MCPSRShutdownLiveness_bug.cfg        -- EnableDrain = FALSE, expects TLC to surface a
\*                                           counter-example to NoOrphanedRequest /
\*                                           ShutdownCompletesCleanly.

EXTENDS PSRShutdownLiveness

(***********************************************************************************************)
(* State constraint -- caps the search space so TLC terminates quickly.                          *)
(***********************************************************************************************)

\* Bound the number of in-flight + resolved requests so the model stays finite.
RequestsBound ==
    Cardinality(ObservedRequests) <= Cardinality(Requests)

\* Limit queue length so we don't spend cycles exploring deep FIFO permutations.
QueueLengthBound == Len(pending) <= 3

=================================================================================================
