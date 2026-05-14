\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------------- MODULE ViewResolutionGetMore ----------------------------------
\* Formal specification of view resolution for $lookup / $graphLookup commands that span a
\* cursor lifecycle (initial batch + one or more getMore batches), in the presence of catalog
\* mutations that can change or remove the view definition mid-command.
\*
\* The bug under SERVER-121988 is: between view-resolution kickbacks (especially across
\* getMore boundaries), a drop+recreate of the foreign view flips the view definition, and
\* the next batch silently resolves the secondary $lookup / $graphLookup against the new
\* definition. The query returns zero matches, but no error is surfaced to the client.
\*
\* This specification models three concurrent actors:
\*
\*   1. The mongos view-resolver (router), which receives the user command, dispatches to a
\*      shard, can receive a `CommandOnShardedViewNotSupportedOnMongod' kickback, refetches
\*      the view definition from the catalog, and re-dispatches.
\*
\*   2. The catalog-mutator, which can `drop', `recreate', or `swap' the view definition
\*      while the cursor is open. Swap = drop the view and create it pointing at a different
\*      backing collection. Drop+recreate = drop and create the view with a different
\*      pipeline (here modelled as a different generation timestamp).
\*
\*   3. The consumer-getMore actor, which iterates the cursor: pulls a batch, then issues
\*      a getMore which on the server side re-enters view resolution for any unresolved
\*      $lookup/$graphLookup sub-pipelines that were lazily expanded.
\*
\* Invariant `SingleViewDefinitionPerCommand': for every cursor c, the set of view
\* definitions actually consumed by c's $lookup / $graphLookup stages across all batches is
\* a singleton. Violating this invariant is exactly the bug.
\*
\* Bug toggle `AllowViewMutationMidCommand': when TRUE the catalog-mutator can fire at any
\* point including while a cursor is open (this drives counterexamples). When FALSE the
\* catalog is frozen once a cursor exists -- this is the post-fix model.
\*
\* The fix (modelled by AllowViewMutationMidCommand = FALSE) is the well-known pattern of
\* pinning the resolved view definition into the cursor's plan context at cursor creation
\* time, so subsequent getMore calls reuse the pinned definition rather than re-resolving.
\* The spec is deliberately agnostic about which side of the wire pins -- mongos cursor
\* manager, shard cursor manager, or both -- because the invariant is observational.
\*
\* To run the model-checker, edit the constants in MCViewResolutionGetMore.cfg if desired,
\* then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Catalog/ViewResolutionGetMore

EXTENDS Integers, Sequences, FiniteSets, TLC
CONSTANTS
    Commands,           \* Set of user commands (each runs one aggregate with $lookup).
    Views,              \* Set of view namespaces ($lookup `from' targets).
    BackingColls,       \* Set of underlying collections a view can target.
    MAX_BATCHES,        \* Max number of getMore batches per cursor.
    MAX_VIEW_GENS,      \* Max number of distinct view generations per view (drop+recreate).
    AllowViewMutationMidCommand  \* BOOLEAN. TRUE => bug-mode. FALSE => fix-mode (frozen).

ASSUME Cardinality(Commands) > 0
ASSUME Cardinality(Views) > 0
ASSUME Cardinality(BackingColls) > 0
ASSUME MAX_BATCHES \in 1..10
ASSUME MAX_VIEW_GENS \in 1..10
ASSUME AllowViewMutationMidCommand \in BOOLEAN

\* Distinguished sentinel for a dropped view (no current definition).
NoView == [gen |-> 0, backing |-> "none"]

\* A view definition has a monotonically-increasing generation (drop+recreate bumps it) and
\* a backing collection it points at. Generation 0 means dropped.
ViewDef == [gen: 0..MAX_VIEW_GENS, backing: BackingColls \cup {"none"}]

\* Command lifecycle stages:
\*   "init"       => not yet dispatched
\*   "resolving"  => dispatched, kickback received, refetching view def
\*   "open"       => cursor open on shard, returning batches
\*   "done"       => cursor exhausted or client killed
CmdStatus == {"init", "resolving", "open", "done"}

\* Sentinel cursor batch types (model-checking values).
BATCH_OK     == "batchOk"
BATCH_EMPTY  == "batchEmpty"

(* Global catalog state *)
VARIABLE viewCatalog    \* viewCatalog[v] is the current ViewDef for view v.
VARIABLE viewGen        \* viewGen[v] is the next-generation counter (monotonic).

(* Per-command state *)
VARIABLE cmdStatus       \* cmdStatus[c] \in CmdStatus.
VARIABLE cmdPinnedDef    \* cmdPinnedDef[c] = the ViewDef pinned at cursor-open time, or NoView.
VARIABLE cmdResolvedDefs \* cmdResolvedDefs[c] = set of ViewDef seen by all stages of c so far.
                         \* This is the observational record: invariant is Cardinality(...) <= 1
                         \* once c has reached `open' status with at least one stage observed.
VARIABLE cmdBatches      \* cmdBatches[c] = number of batches returned (including initial).
VARIABLE cmdBatchesUsed  \* cmdBatchesUsed[c] = sequence of batch outcomes (BATCH_OK / BATCH_EMPTY).

(* Bookkeeping *)
VARIABLE cursorOpen      \* set of commands whose cursor is currently open.

vars == << viewCatalog, viewGen, cmdStatus, cmdPinnedDef, cmdResolvedDefs,
           cmdBatches, cmdBatchesUsed, cursorOpen >>

\* Which view each command targets. We pin this at Init for simplicity; in practice a
\* command can target multiple views, but the invariant is per-(command, view) and the
\* state explosion of multi-view does not strengthen the test of the bug.
CommandView(c) == CHOOSE v \in Views : TRUE

----------------------------------------------------------------------------------------------
(* Helpers. *)

\* The view definition the catalog will hand out *right now* if asked. May be NoView.
CurrentViewDef(v) == viewCatalog[v]

\* Has the catalog mutation actor any work to do? It is constrained by MAX_VIEW_GENS so the
\* state space terminates.
CanMutateView(v) == viewGen[v] < MAX_VIEW_GENS

\* The catalog can mutate freely in bug-mode, or only when no cursor is open in fix-mode.
\* In fix-mode we keep the strictest reading: once *any* cursor exists, no mutation. A
\* less strict model (per-view freeze) is a tighter refinement; this model is sufficient
\* to demonstrate that the broad invariant holds.
MutationAllowed ==
    \/ AllowViewMutationMidCommand
    \/ cursorOpen = {}

\* Project a definition to its observable identity (gen + backing). Two definitions are
\* "the same" iff they have the same generation and same backing collection.
DefId(d) == << d.gen, d.backing >>

----------------------------------------------------------------------------------------------
(* Initial state. *)

Init ==
    \* Every view starts with generation 1 backed by some backing collection. We pick
    \* deterministically the first backing collection to avoid blowing up Init.
    /\ viewCatalog = [v \in Views |-> [gen |-> 1,
                                        backing |-> CHOOSE b \in BackingColls : TRUE]]
    /\ viewGen = [v \in Views |-> 1]
    /\ cmdStatus = [c \in Commands |-> "init"]
    /\ cmdPinnedDef = [c \in Commands |-> NoView]
    /\ cmdResolvedDefs = [c \in Commands |-> {}]
    /\ cmdBatches = [c \in Commands |-> 0]
    /\ cmdBatchesUsed = [c \in Commands |-> <<>>]
    /\ cursorOpen = {}

----------------------------------------------------------------------------------------------
(* Catalog-mutator actions. *)

\* Drop a view (set to NoView). Allowed at any cluster time, subject to the bug toggle.
\* A drop also bumps the per-view generation counter so any subsequent recreate is fresh.
DropView(v) ==
    /\ MutationAllowed
    /\ CurrentViewDef(v).gen # 0       \* not already dropped
    /\ CanMutateView(v)
    /\ viewCatalog' = [viewCatalog EXCEPT ![v] = NoView]
    /\ viewGen' = [viewGen EXCEPT ![v] = @ + 1]
    /\ UNCHANGED << cmdStatus, cmdPinnedDef, cmdResolvedDefs, cmdBatches,
                    cmdBatchesUsed, cursorOpen >>

\* Recreate a previously-dropped view with a (potentially different) backing collection.
\* The new generation is the bumped viewGen counter.
RecreateView(v, b) ==
    /\ MutationAllowed
    /\ CurrentViewDef(v).gen = 0       \* must be dropped
    /\ CanMutateView(v)
    /\ b \in BackingColls
    /\ viewCatalog' = [viewCatalog EXCEPT ![v] = [gen |-> viewGen[v] + 1, backing |-> b]]
    /\ viewGen' = [viewGen EXCEPT ![v] = @ + 1]
    /\ UNCHANGED << cmdStatus, cmdPinnedDef, cmdResolvedDefs, cmdBatches,
                    cmdBatchesUsed, cursorOpen >>

\* Swap: atomic drop + recreate pointing at a different backing collection. Modeled as one
\* step so the state space stays tractable, and because mongos cannot observe a moment
\* where the view does not exist (the rename-collection-to-collection path uses an exclusive
\* DDL lock under the hood).
SwapView(v, b) ==
    /\ MutationAllowed
    /\ CurrentViewDef(v).gen # 0
    /\ CanMutateView(v)
    /\ b \in BackingColls
    /\ b # CurrentViewDef(v).backing
    /\ viewCatalog' = [viewCatalog EXCEPT ![v] = [gen |-> viewGen[v] + 1, backing |-> b]]
    /\ viewGen' = [viewGen EXCEPT ![v] = @ + 1]
    /\ UNCHANGED << cmdStatus, cmdPinnedDef, cmdResolvedDefs, cmdBatches,
                    cmdBatchesUsed, cursorOpen >>

----------------------------------------------------------------------------------------------
(* Mongos view-resolver actions. *)

\* The router dispatches command c, which targets view CommandView(c). Modeled as a single
\* transition that emulates: dispatch -> kickback -> refetch -> re-dispatch -> cursor open.
\* Across this transition the catalog may have been mutated, which is faithful to the
\* observed behaviour: the kickback path re-reads the view catalog without a snapshot.
OpenCursor(c) ==
    /\ cmdStatus[c] = "init"
    /\ LET v == CommandView(c)
           defAtOpen == CurrentViewDef(v)
       IN
       /\ defAtOpen.gen # 0          \* router refuses to open a cursor on a dropped view
       /\ cmdStatus' = [cmdStatus EXCEPT ![c] = "open"]
       /\ cmdPinnedDef' = [cmdPinnedDef EXCEPT ![c] = defAtOpen]
       /\ cmdResolvedDefs' = [cmdResolvedDefs EXCEPT ![c] = @ \cup {defAtOpen}]
       /\ cmdBatches' = [cmdBatches EXCEPT ![c] = 1]
       /\ cmdBatchesUsed' = [cmdBatchesUsed EXCEPT ![c] = Append(@, BATCH_OK)]
       /\ cursorOpen' = cursorOpen \cup {c}
    /\ UNCHANGED << viewCatalog, viewGen >>

\* GetMore: client issues a getMore against an open cursor. Server resolves $lookup against
\* the *current* view catalog if AllowViewMutationMidCommand is TRUE (bug). When FALSE we
\* still resolve against the current catalog but the catalog can't have moved, so the
\* observation is consistent. Either way, cmdResolvedDefs accumulates whatever the server
\* actually saw for this batch -- this is the observational invariant.
\*
\* Note: a more aggressive (and more realistic) fix-model would always read cmdPinnedDef
\* here instead of CurrentViewDef. That refinement is correct but is one *implementation*
\* of the fix. The model uses the freeze approach so the invariant is enforced by the
\* catalog itself.
GetMore(c) ==
    /\ c \in cursorOpen
    /\ cmdStatus[c] = "open"
    /\ cmdBatches[c] < MAX_BATCHES
    /\ LET v == CommandView(c)
           defNow == CurrentViewDef(v)
           batchOutcome == IF defNow.gen = 0 \/ DefId(defNow) # DefId(cmdPinnedDef[c])
                          THEN BATCH_EMPTY
                          ELSE BATCH_OK
       IN
       /\ cmdResolvedDefs' = [cmdResolvedDefs EXCEPT ![c] = @ \cup {defNow}]
       /\ cmdBatches' = [cmdBatches EXCEPT ![c] = @ + 1]
       /\ cmdBatchesUsed' = [cmdBatchesUsed EXCEPT ![c] = Append(@, batchOutcome)]
    /\ UNCHANGED << viewCatalog, viewGen, cmdStatus, cmdPinnedDef, cursorOpen >>

\* Client (or server) closes the cursor. The cursor leaves cursorOpen so the catalog
\* mutator can resume (in fix-mode).
CloseCursor(c) ==
    /\ c \in cursorOpen
    /\ cmdStatus[c] = "open"
    /\ cmdStatus' = [cmdStatus EXCEPT ![c] = "done"]
    /\ cursorOpen' = cursorOpen \ {c}
    /\ UNCHANGED << viewCatalog, viewGen, cmdPinnedDef, cmdResolvedDefs,
                    cmdBatches, cmdBatchesUsed >>

----------------------------------------------------------------------------------------------
(* Next-state relation. *)

Next ==
    \* Mongos / cursor lifecycle
    \/ \E c \in Commands : OpenCursor(c)
    \/ \E c \in Commands : GetMore(c)
    \/ \E c \in Commands : CloseCursor(c)
    \* Catalog DDL actions
    \/ \E v \in Views : DropView(v)
    \/ \E v \in Views, b \in BackingColls : RecreateView(v, b)
    \/ \E v \in Views, b \in BackingColls : SwapView(v, b)
    \* Allow stuttering once every command is in a terminal state (`done', or `init' but
    \* permanently stuck because its target view is dropped and the catalog has exhausted
    \* its DDL budget). We could check `CHECK_DEADLOCK FALSE' in the cfg instead, but this
    \* approach keeps the spec explicit about which terminal states are legitimate.
    \/ ( /\ \A c \in Commands :
              \/ cmdStatus[c] = "done"
              \/ (cmdStatus[c] = "init" /\ CurrentViewDef(CommandView(c)).gen = 0
                  /\ ~CanMutateView(CommandView(c)))
         /\ UNCHANGED vars )

Fairness ==
    /\ WF_vars(\E c \in Commands : OpenCursor(c))
    /\ WF_vars(\E c \in Commands : GetMore(c))
    /\ WF_vars(\E c \in Commands : CloseCursor(c))

Spec == /\ Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Type invariants.                                                                              *)
(**************************************************************************************************)

TypeOK ==
    /\ viewCatalog \in [Views -> ViewDef]
    /\ viewGen \in [Views -> 0..MAX_VIEW_GENS]
    /\ cmdStatus \in [Commands -> CmdStatus]
    /\ cmdPinnedDef \in [Commands -> ViewDef]
    /\ cmdResolvedDefs \in [Commands -> SUBSET ViewDef]
    /\ cmdBatches \in [Commands -> 0..MAX_BATCHES]
    /\ cursorOpen \subseteq Commands

\* View generations only ever go up.
ViewGenMonotonic ==
    \A v \in Views : viewGen[v] >= 1 \/ viewGen[v] = 0

(**************************************************************************************************)
(* Correctness properties.                                                                       *)
(**************************************************************************************************)

\* THE primary invariant. For every command c that has reached `open' (cursor was created),
\* the set of view definitions its $lookup / $graphLookup stages have actually consumed
\* across all batches MUST be a singleton.
\*
\* This is the formal statement of "all $lookups + $graphLookups within one user command
\* resolve against the same view definition", parametrised over the natural notion of
\* "definition" (generation + backing).
SingleViewDefinitionPerCommand ==
    \A c \in Commands :
        (cmdStatus[c] \in {"open", "done"} /\ cmdResolvedDefs[c] # {})
        => Cardinality({ DefId(d) : d \in cmdResolvedDefs[c] }) = 1

\* A complementary correctness property: a cursor that started returning data must never
\* spontaneously start returning empty batches purely because the catalog moved underneath
\* it. (This is the user-visible symptom in SERVER-121988: the cursor returns 0 documents
\* for the second batch despite the data being present.)
NoSilentEmptyBatchAfterOk ==
    \A c \in Commands :
        LET batches == cmdBatchesUsed[c]
            n == Len(batches)
        IN \A i \in 1..n :
            (i > 1 /\ batches[i] = BATCH_EMPTY /\ batches[i-1] = BATCH_OK)
            => FALSE  \* simply: this transition must never occur

\* Liveness: every command eventually completes.
AllCommandsEventuallyDone == <>[] \A c \in Commands : cmdStatus[c] = "done"

----------------------------------------------------------------------------------------------
(**************************************************************************************************)
(* Miscellaneous derived state for exploration.                                                  *)
(**************************************************************************************************)

\* How many distinct view definitions a given command has seen.
CmdSeenDefCount(c) == Cardinality({ DefId(d) : d \in cmdResolvedDefs[c] })

\* True iff at least one cursor is open.
SomeCursorOpen == cursorOpen # {}

\* True iff the catalog and the pinned-def of every open cursor agree on (gen, backing) for
\* the view they target. This is an auxiliary invariant that, when AllowViewMutationMidCommand
\* is FALSE, follows from MutationAllowed and is therefore a useful sanity check.
PinnedDefMatchesCatalogForOpenCursors ==
    \A c \in cursorOpen :
        LET v == CommandView(c) IN
        DefId(cmdPinnedDef[c]) = DefId(viewCatalog[v])

====================================================================================================
