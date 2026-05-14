---------------------------------- MODULE MCBulkInsertDDLAtomicity ------------------------------
\* Model-checking harness for BulkInsertDDLAtomicity.tla. Defines symmetry, state-space caps,
\* and bait operators used to surface the SERVER-95924 sub-batch incarnation split.

EXTENDS BulkInsertDDLAtomicity

\* Symmetry over the set of bulk-insert clients. Sub-batches are positional and not symmetric.
Symmetry == Permutations(Bulks)

\* Hard cap to bound the search. Once every bulk has finished AND no more DDLs are pending, there
\* is nothing left to learn.
StateConstraint ==
    \/ \E b \in Bulks : ~BulkDone(b)
    \/ ddlCount < DDLBudget

\* Bait: produce a counterexample trace where two sub-batches of the SAME bulk recorded different
\* incarnations. Under LOCK_SCOPE = SUBBATCH this should fire quickly and exhibits SERVER-95924.
\* Under LOCK_SCOPE = BULK this should remain unfalsified -- the fix forecloses the split.
BaitSubBatchIncarnationSplit ==
    \A b \in Bulks :
        \A i, j \in 1..Len(bulkLog[b]) :
            bulkLog[b][i].incarnation = bulkLog[b][j].incarnation

\* Bait: produce a counterexample trace where a bulk starts on a DROPPED namespace and a later
\* sub-batch lands on a fresh incarnation -- the rename-split shape from the ticket description.
BaitDropToLiveSplit ==
    \A b \in Bulks :
        ~ \E i, j \in 1..Len(bulkLog[b]) :
            /\ i < j
            /\ bulkLog[b][i].incarnation = DROPPED
            /\ bulkLog[b][j].incarnation # DROPPED

\* Bait: produce a trace where every bulk finishes coherently (all sub-batches on one
\* incarnation). Useful to confirm the green configuration permits success.
BaitHappyPath ==
    ~ \A b \in Bulks :
        /\ BulkDone(b)
        /\ \A i, j \in 1..Len(bulkLog[b]) :
            bulkLog[b][i].incarnation = bulkLog[b][j].incarnation

====================================================================================================
