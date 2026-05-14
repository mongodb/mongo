\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------ MODULE PageServerReaderStaleSorting ------------------------
\* Formal model of PageServerReader::_sortPageServerCandidates() under step-up, when
\* remote cell frontiers are not yet known by the stepping-up node.
\*
\* Bug (production sorting rule, modelled by `PreferLocalUnconditional'):
\*   Score(c) == lagInLsns(c) + (IF c \in Remote THEN Threshold ELSE 0)
\*   where an unknown frontier maps to Timestamp::max().getSecs() ~= 2^32 - 1.
\* On step-up, remote frontiers are still unknown (huge lag) while the local
\* frontier has a known stale value (tiny lag). The local cell wins sorting and
\* serves a read it cannot satisfy, then every subsequent attempt eats a 14s
\* GetPageAtLSN timeout before falling back to remote. Step-up exceeds the task
\* idle timeout.
\*
\* Fix (modelled by `PreferKnownOverUnknown'): an "unknown" remote frontier is
\* treated as DistinctFromBehind. When the local frontier is known to be < min
\* known-remote-frontier (or all remote frontiers are unknown), the reader does
\* NOT prefer local; it picks a remote with a known frontier when one exists,
\* and only falls through to local when there is positive evidence that local
\* is at least as fresh as the requested LSN.
\*
\* To run the model-checker, first edit the constants in
\* MCPageServerReaderStaleSorting.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Disagg/PageServerReaderStaleSorting/MCPageServerReaderStaleSorting
\*
\* And to reproduce the bug (expected counter-example on NoServingStaleReadFromLocal):
\*     ./model-check.sh Disagg/PageServerReaderStaleSorting/BugMCPageServerReaderStaleSorting

EXTENDS Integers, FiniteSets, Sequences, TLC

CONSTANTS
    RemoteCells,       \* Set of remote cell IDs (e.g. {r1, r2}).
    LocalCell,         \* The local cell ID (singleton, distinct from RemoteCells).
    MaxLSN,            \* Bound on requested LSN and committed-LSN values.
    SortingRule        \* Either "PreferLocalUnconditional" (bug) or
                       \* "PreferKnownOverUnknown" (fix).

\* Sentinel value for "remote cell frontier not yet known by this reader".
\* In production this is encoded as Timestamp::max().getSecs() (~ 2^32 - 1).
UnknownFrontier == "UnknownFrontier"

\* Sentinel for "reader has not yet picked a candidate".
NoPick == "NoPick"

ASSUME LocalCell \notin RemoteCells
ASSUME Cardinality(RemoteCells) >= 1
ASSUME MaxLSN \in Nat /\ MaxLSN >= 2
ASSUME SortingRule \in {"PreferLocalUnconditional", "PreferKnownOverUnknown"}

Cells == RemoteCells \cup {LocalCell}

\* --- Variables -------------------------------------------------------------

\* frontier[c]: latest LSN materialized at cell c as known by THIS reader.
\* Values: a Nat in 0..MaxLSN, or UnknownFrontier.
VARIABLE frontier

\* The "ground truth" latest committed LSN in the cluster. Monotonically
\* non-decreasing; remote cells eventually catch up to this LSN.
VARIABLE committedLSN

\* The LSN the reader's current request needs satisfied.
VARIABLE requestedLSN

\* Whether step-up has occurred yet. Pre step-up: local frontier reflects
\* truth and remote frontiers are gossiped. Post step-up: local frontier is
\* frozen at the value it had at step-up time (becomes stale as truth
\* advances), and remote frontiers reset to UnknownFrontier until gossip
\* refreshes them.
VARIABLE steppedUp

\* The cell the reader has chosen to send the page request to (NoPick before
\* it has run the sort).
VARIABLE pick

\* History trace of picks made post step-up. Used to express the safety
\* invariant: no element of this sequence may be a stale-local pick.
VARIABLE pickHistory

vars == <<frontier, committedLSN, requestedLSN, steppedUp, pick, pickHistory>>

\* --- Helpers ---------------------------------------------------------------

IsKnown(c)   == frontier[c] /= UnknownFrontier
IsUnknown(c) == frontier[c] = UnknownFrontier

\* The set of remote cells whose frontier is known by this reader.
KnownRemotes == { c \in RemoteCells : IsKnown(c) }

\* Minimum known remote frontier. Defined only when KnownRemotes is non-empty.
\* If empty, callers must guard accordingly.
MinKnownRemoteFrontier ==
    IF KnownRemotes = {} THEN 0
    ELSE CHOOSE v \in { frontier[c] : c \in KnownRemotes } :
            \A c2 \in KnownRemotes : v <= frontier[c2]

\* A cell "can serve" requestedLSN iff its frontier is known AND that
\* frontier is >= requestedLSN.
CanServe(c) == IsKnown(c) /\ frontier[c] >= requestedLSN

\* The local cell is "definitely stale relative to known remotes" iff:
\*   - the local frontier is known (we have a value to compare), AND
\*   - it is strictly less than the minimum known-remote frontier.
LocalDefinitelyStale ==
    /\ IsKnown(LocalCell)
    /\ KnownRemotes /= {}
    /\ frontier[LocalCell] < MinKnownRemoteFrontier

\* --- Sorting rule: production (bug) ---------------------------------------
\* Score(c) = lag(c) + (IF c \in Remote THEN Threshold ELSE 0). With unknown
\* frontiers encoded as Timestamp::max() the local cell (known but stale)
\* always sorts first. We do not need to compute Score numerically; we just
\* express the head-of-list behaviour: prefer local always when local is known.
PickBuggy ==
    IF IsKnown(LocalCell) THEN LocalCell
    ELSE IF KnownRemotes /= {} THEN CHOOSE c \in KnownRemotes : TRUE
    ELSE CHOOSE c \in RemoteCells : TRUE   \* All unknown: arbitrary remote.

\* --- Sorting rule: fix ----------------------------------------------------
\* "Unknown remote frontier" is treated as a third state, distinct from
\* "remote behind". The reader does NOT prefer local when:
\*   (a) any remote frontier is unknown (we cannot prove local is best), OR
\*   (b) local is provably staler than min known-remote frontier.
\* In both cases, prefer a known-remote cell if one exists; otherwise an
\* arbitrary remote (so a remote gossip path is exercised, not a doomed
\* local read).
PickFixed ==
    IF LocalDefinitelyStale
        THEN IF KnownRemotes /= {}
                THEN CHOOSE c \in KnownRemotes : TRUE
                ELSE CHOOSE c \in RemoteCells : TRUE
    ELSE IF \E c \in RemoteCells : IsUnknown(c)
        THEN IF KnownRemotes /= {}
                THEN CHOOSE c \in KnownRemotes : TRUE
                ELSE CHOOSE c \in RemoteCells : TRUE
    ELSE \* All remote frontiers known and local not provably stale: keep local
         \* if it can serve, else fall back to best remote.
         IF IsKnown(LocalCell) /\ CanServe(LocalCell)
            THEN LocalCell
            ELSE IF KnownRemotes /= {}
                    THEN CHOOSE c \in KnownRemotes : TRUE
                    ELSE CHOOSE c \in RemoteCells : TRUE

Pick == IF SortingRule = "PreferLocalUnconditional" THEN PickBuggy ELSE PickFixed

\* --- Initial state --------------------------------------------------------
\* Pre step-up: local cell has frontier 0 (will become stale post step-up).
\* Remote cells start with arbitrary known frontiers in 0..MaxLSN so we
\* explore the case where some remotes have caught up already.
Init ==
    /\ committedLSN = 0
    /\ requestedLSN = 0
    /\ steppedUp    = FALSE
    /\ pick         = NoPick
    /\ pickHistory  = <<>>
    /\ frontier     = [ c \in Cells |->
                          IF c = LocalCell THEN 0 ELSE UnknownFrontier ]

\* --- Actions --------------------------------------------------------------

\* The cluster advances and a write commits at the new LSN. Remote cells'
\* known-by-reader frontier does NOT auto-update; the reader has to learn
\* via gossip (RefreshRemoteFrontier below).
AdvanceCommitted ==
    /\ committedLSN < MaxLSN
    /\ committedLSN' = committedLSN + 1
    /\ UNCHANGED <<frontier, requestedLSN, steppedUp, pick, pickHistory>>

\* The reader's node receives an RPC and requests a page at the latest LSN.
\* This raises requestedLSN to committedLSN, so post step-up the local
\* frontier (frozen at step-up time) is strictly less than requestedLSN.
SetRequestedLSN ==
    /\ pick = NoPick                  \* Only set a new requested LSN between picks.
    /\ requestedLSN' = committedLSN
    /\ UNCHANGED <<frontier, committedLSN, steppedUp, pick, pickHistory>>

\* Step-up: this reader becomes primary. Remote-cell frontiers reset to
\* UnknownFrontier (the stepping-up node has not consumed segment-2 log
\* entries that publish them); local frontier is left at its current value
\* (which becomes stale as committedLSN advances afterwards).
StepUp ==
    /\ ~steppedUp
    /\ steppedUp' = TRUE
    /\ frontier'  = [ c \in Cells |->
                        IF c \in RemoteCells THEN UnknownFrontier
                        ELSE frontier[c] ]
    /\ UNCHANGED <<committedLSN, requestedLSN, pick, pickHistory>>

\* Gossip / matCheckpoint arrives: the reader learns one remote cell's
\* current frontier (set equal to committedLSN at the time of refresh, to
\* model a remote that is caught up).
RefreshRemoteFrontier(c) ==
    /\ c \in RemoteCells
    /\ IsUnknown(c)
    /\ frontier' = [ frontier EXCEPT ![c] = committedLSN ]
    /\ UNCHANGED <<committedLSN, requestedLSN, steppedUp, pick, pickHistory>>

\* The reader runs _sortPageServerCandidates() and picks the head of the
\* sorted list. We only require that a pick is recorded into history once
\* steppedUp is TRUE (the bug only fires during step-up sort), but we also
\* allow pre-step-up picks so the model covers the pre-bug normal path.
SortAndPick ==
    /\ pick = NoPick
    /\ requestedLSN > 0               \* Skip degenerate LSN=0 picks.
    /\ pick' = Pick
    /\ pickHistory' = IF steppedUp THEN Append(pickHistory, Pick) ELSE pickHistory
    /\ UNCHANGED <<frontier, committedLSN, requestedLSN, steppedUp>>

\* Finish handling one request: reader resets pick so a fresh sort can run.
\* In production this is the point where, on a TimedOutWaitForMaterialization
\* response, the fix proposed in the ticket would deprioritize the failing
\* cell. We do not model the timed-out penalty path here because the safety
\* invariant we want is upstream of it (no stale-local pick at sort time).
ClearPick ==
    /\ pick /= NoPick
    /\ pick' = NoPick
    /\ UNCHANGED <<frontier, committedLSN, requestedLSN, steppedUp, pickHistory>>

Next ==
    \/ AdvanceCommitted
    \/ SetRequestedLSN
    \/ StepUp
    \/ \E c \in RemoteCells : RefreshRemoteFrontier(c)
    \/ SortAndPick
    \/ ClearPick

Spec == Init /\ [][Next]_vars

\* --- Safety invariant -----------------------------------------------------
\* A "stale local pick" is a post-step-up pick whose chosen cell is LocalCell
\* AND the local frontier known to the reader is strictly less than the
\* minimum known-remote frontier (i.e., we have positive evidence that a
\* remote cell is strictly fresher than local, yet picked local anyway).
\*
\* If no remote frontier is known yet, we cannot prove "local is staler than
\* a known remote"; that situation is what the FIX must avoid degenerating
\* into a doomed local read, which is captured by a complementary check
\* below.
StaleLocalPickAt(i) ==
    /\ pickHistory[i] = LocalCell
    /\ IsKnown(LocalCell)
    /\ KnownRemotes /= {}
    /\ frontier[LocalCell] < MinKnownRemoteFrontier

\* And a "doomed local pick" is a post-step-up pick that landed on LocalCell
\* when the reader had ZERO evidence about remote frontiers AND local cannot
\* serve the requested LSN. The bug also produces this case (all remotes
\* unknown -> Score(remote)=2^32, Score(local)=small; local wins; local
\* lacks the LSN; the request will eat the timeout).
DoomedLocalPickAt(i) ==
    /\ pickHistory[i] = LocalCell
    /\ ~CanServe(LocalCell)
    /\ \A c \in RemoteCells : IsUnknown(c)

\* The combined invariant the fix must preserve.
NoServingStaleReadFromLocal ==
    \A i \in 1..Len(pickHistory) :
        /\ ~StaleLocalPickAt(i)
        /\ ~DoomedLocalPickAt(i)

\* --- Type invariant -------------------------------------------------------
TypeOK ==
    /\ committedLSN \in 0..MaxLSN
    /\ requestedLSN \in 0..MaxLSN
    /\ steppedUp \in BOOLEAN
    /\ pick \in (Cells \cup {NoPick})
    /\ pickHistory \in Seq(Cells)
    /\ frontier \in [ Cells -> (0..MaxLSN) \cup {UnknownFrontier} ]

=============================================================================
