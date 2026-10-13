------------------------- MODULE MCPipelinedOplog ----------------------------
\* Overlapping oplog batches with repeated keys and inline op 4.
\* Seven ops, two keys, two workers, bounds of three.
\*
\* Run from this directory:
\* java -cp ~/bin/tla2tools.jar tlc2.TLC -workers auto MCPipelinedOplog

EXTENDS PipelinedOplog

CONSTANTS k1, k2, wA, wB

----
\* Model inputs

MC_NumOps        == 7
MC_Keys          == {k1, k2}
MC_Workers       == {wA, wB}

\* A sequence is also a function on 1..Len: index MC_OpKey[i] is operation i's key.
MC_OpKey         == <<k1, k2, k1, k1, k2, k1, k2>>   \* op i -> MC_OpKey[i]
\* k1 always routes to wA, and k2 to wB.
MC_WorkerOf      == (k1 :> wA) @@ (k2 :> wB)

MC_SerializedOps == {4}
MC_MaxBatchSize  == 3
MC_MaxQueueDepth == 3
MC_MaxInflight   == 3

=============================================================================
