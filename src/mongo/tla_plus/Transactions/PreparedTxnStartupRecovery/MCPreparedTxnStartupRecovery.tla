------------------------ MODULE MCPreparedTxnStartupRecovery -----------------------------------
\* Model-checking module for PreparedTxnStartupRecovery (SERVER-115355).

EXTENDS PreparedTxnStartupRecovery

(**************************************************************************************************)
(* State constraints to keep the model finite.                                                    *)
(**************************************************************************************************)

\* Bound the total oplog length so TLC does not chase unbounded append sequences.
OplogLenBound == Len(oplog) <= 8

================================================================================================
