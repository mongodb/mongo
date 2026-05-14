---- MODULE MCOplogTruncationLiveness ----
\* This module defines OplogTruncationLiveness.tla constants/constraints
\* for model-checking. See OplogTruncationLiveness.tla for instructions.

EXTENDS OplogTruncationLiveness

\* Keep the state space small. The bug manifests at any size where a
\* single requested truncation exceeds CacheBudget, so we don't need
\* large numeric ranges.
StateConstraint ==
    /\ oplogSize   <= MaxOplogSize
    /\ attemptSize <= MaxOplogSize
    /\ truncated   <= MaxOplogSize
=============================================================================
