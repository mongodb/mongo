------------------- MODULE MCBucketCatalogConcurrentDrop ---------------------
\* Model-checking module for BucketCatalogConcurrentDrop.
\*
\* The default cfg (MCBucketCatalogConcurrentDrop.cfg) wires the model in the
\* "bug" configuration: AllowInterleavedDropPhases = TRUE, which lets an
\* insert thread re-emplace executionStats or open buckets for a uuid whose
\* drop is mid-flight. TLC must report a counterexample violating
\* CatalogConsistentPostDrop.
\*
\* To exercise the "fixed" configuration (drop's two phases serialised
\* w.r.t. insert), edit the cfg's CONSTANT line to set
\* AllowInterleavedDropPhases <- FALSE - the invariants must then hold.

EXTENDS BucketCatalogConcurrentDrop

(******************************************************************************)
(* State constraints. Bound TLC's exploration of the state space.             *)
(******************************************************************************)

\* Cap drops at MaxDrops (already enforced by ReleaseStats), and cap the
\* number of distinct in-flight states by limiting droppedHistory length.
HistoryBound == Len(droppedHistory) <= MaxDrops

(******************************************************************************)
(* Counterexample baits. Toggle in the cfg INVARIANT block to confirm TLC     *)
(* would explode if these were really invariants.                             *)
(******************************************************************************)

\* Never holds in the bug model: TLC can always race insert into a dropped
\* uuid.
BaitExecutionStatsNeverDirty == executionStats \cap DroppedUuids = {}

==============================================================================
