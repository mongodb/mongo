---- MODULE MCMajorityAckUnderPartition ----
\* Model-check parameters for MajorityAckUnderPartition.tla.
\* See MajorityAckUnderPartition.tla for the bug specification
\* (SERVER-101041).

EXTENDS MajorityAckUnderPartition

\* The smallest configuration that exhibits the bug from the ticket:
\* 3-node replica set, single key, two distinct values, two writes
\* (one legitimate majority commit + one no-op short-circuit).

==============================================================================
