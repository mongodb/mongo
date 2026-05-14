\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---- MODULE MCResumeTokenArming ----
\* Model-check configuration wrapper for ResumeTokenArming.tla.
\*
\* See ResumeTokenArming.tla for the spec narrative.
\*
\* Run from src/mongo/tla_plus:
\*     ./model-check.sh ChangeStreams/ResumeTokenArming
\*
\* The shipped configuration (MCResumeTokenArming.cfg) sets
\* UseStartAt = FALSE, which causes TLC to produce a counterexample
\* trace of the lazy-arming event-loss bug. To verify the fix, toggle
\* UseStartAt = TRUE in the cfg and re-run; NoEventLoss will hold
\* across the full reachable state space.

EXTENDS ResumeTokenArming

\* State-space bounds. Kept tiny for sub-minute model-check wall time.
\* MaxClock = 4 and MaxEvents = 2 give roughly a few thousand reachable
\* states with one consumer thread, more than enough to surface the
\* event-loss counterexample on the lazy path.
StateConstraint ==
    /\ clock <= MaxClock
    /\ Len(oplog) <= MaxEvents

=============================================================================
