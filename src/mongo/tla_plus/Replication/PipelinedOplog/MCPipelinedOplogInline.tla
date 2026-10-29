---------------------- MODULE MCPipelinedOplogInline -------------------------
\* Inline ops at the start, consecutively, and at the end.
\* Queue depth and in-flight bounds of one exercise backpressure.
\*
\* Run manually from this directory; Evergreen runs only MCPipelinedOplog.
\* java -cp ~/bin/tla2tools.jar tlc2.TLC -workers auto \
\*   -config MCPipelinedOplog.cfg MCPipelinedOplogInline

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

MC_SerializedOps == {1, 4, 5, 7}
MC_MaxBatchSize  == 3
MC_MaxQueueDepth == 1
MC_MaxInflight   == 1

=============================================================================
