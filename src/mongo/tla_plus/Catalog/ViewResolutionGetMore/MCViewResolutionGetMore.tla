---------------------------------- MODULE MCViewResolutionGetMore ----------------------------------
\* This module defines ViewResolutionGetMore.tla constants/constraints for model-checking.

EXTENDS ViewResolutionGetMore

(**************************************************************************************************)
(* State constraints.                                                                            *)
(**************************************************************************************************)

\* Cap exploration: stop once every command is `done' (or once we've already exhausted the
\* configured DDL budget via MAX_VIEW_GENS). Keeps state space bounded.
StateConstraint ==
    /\ \E c \in Commands : cmdStatus[c] # "done"

\* Symmetry over commands, views, and backing collections. We deliberately do NOT symmetry
\* over generations because the invariant relies on the monotonic ordering of gens.
Symmetry == Permutations(Commands) \union Permutations(Views) \union Permutations(BackingColls)

(**************************************************************************************************)
(* Counterexample baits (for trace generation).                                                  *)
(**************************************************************************************************)

\* Bait an execution that reaches at least one open cursor, useful for verifying the model
\* actually exercises the cursor lifecycle.
BaitHasOpenCursor == ~(\E c \in Commands : cmdStatus[c] = "open")

\* Bait an execution that drops a view at least once. Useful in bug-mode to ensure the
\* catalog mutator fires.
BaitDropOccurred == ~(\E v \in Views : viewCatalog[v] = NoView)

\* Bait an execution where a command sees more than one view definition. In bug-mode this
\* fires; in fix-mode it should NEVER fire (SingleViewDefinitionPerCommand subsumes it).
BaitTwoDefsSeen ==
    ~ \E c \in Commands : Cardinality({ DefId(d) : d \in cmdResolvedDefs[c] }) > 1

====================================================================================================
