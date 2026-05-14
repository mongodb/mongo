---------------------------- MODULE MCTimeSeriesCollModCritSec ------------------------------------
\* Model-checking harness for TimeSeriesCollModCritSec.tla. Constants/symmetry/baits.
\*
\* Flip FIX_ENABLED in MCTimeSeriesCollModCritSec.cfg between FALSE (reproduce SERVER-125921 as a
\* counterexample to NoStrayCriticalSectionOnTermination) and TRUE (verify the proposed fix).

EXTENDS TimeSeriesCollModCritSec

(* State constraints / symmetry *)

\* Symmetry over participant shards: ordering of ApplyCollModOnShard / ReleaseCritSecOnShard does
\* not matter for the safety invariants.
Symmetry == Permutations(Shards)

(*  Counterexample baits.                                                                        *)

\* Bait: at least one non-retriable abort fires. Used to confirm the model is exercising the
\* aborting branches and not just the happy path.
BaitAbortFires == aborts = 0

\* Bait: the coordinator actually drives all shards through the apply path. Used to confirm the
\* model can reach kDone with every participant having applied.
BaitAllShardsApplied == \E s \in Shards : sCollModApplied[s] = FALSE

\* Bait: model can reach Aborted with the fix wired up.
BaitAbortedTerminal == phase # PhaseAborted

====================================================================================================
