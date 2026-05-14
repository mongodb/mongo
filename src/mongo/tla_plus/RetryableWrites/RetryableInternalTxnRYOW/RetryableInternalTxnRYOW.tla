\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE RetryableInternalTxnRYOW -------------------------
\* Formal model of retryable internal transactions that span multiple shards and
\* the short-circuit path that returns a cached response when a statement is seen
\* with a stmtId that the shard has already executed.
\*
\* A retryable write command from a client can be rewritten by the router as an
\* internal transaction with multiple write statements. Only statements carrying
\* a stmtId >= 0 are retryable; statements with stmtId = -1 are non-retryable and
\* must be re-executed on every retry. Today the router will short-circuit and
\* skip non-retryable statements as soon as the FIRST retryable statement comes
\* back with a "retriedStmtIds" marker from the shard. If the original internal
\* transaction had committed on the shard owning the retryable statement but had
\* NOT yet committed on the shard owning the non-retryable statement, the
\* short-circuit causes the retry to return success without executing the
\* non-retryable statement on the second shard. A read issued in the same
\* session immediately afterward (without afterClusterTime) can fail to see the
\* effect of the non-retryable statement — a read-your-own-writes violation.
\*
\* This spec models:
\*   - one logical client session
\*   - two routers (mongos0, mongos1)
\*   - two shards (shard0, shard1); shard0 owns the retryable statement, shard1
\*     owns the non-retryable statement
\*   - retryable write rewritten to internal txn with two statements
\*   - per-shard stmtId history (durable participants record stmtIds already
\*     executed once the internal transaction commits on that shard)
\*   - two-phase commit pictured as two independent shard-commit actions that
\*     can interleave with the retry path
\*   - a "short-circuit on partial txn" knob that toggles the buggy behavior
\*
\* To run the model-checker against the safe configuration:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh RetryableWrites/RetryableInternalTxnRYOW
\*
\* To exercise the bug configuration (which produces a counterexample to
\* ReadYourOwnWrites), copy MC_bug.cfg over MCRetryableInternalTxnRYOW.cfg and
\* re-run the script. README.md walks through both runs.

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    \* Whether the short-circuit can fire when the shard for the non-retryable
    \* statement has not yet observed the original internal transaction commit.
    \* When FALSE (the safe configuration), the router blocks the retry on the
    \* uncommitted shard and re-runs the non-retryable statement; when TRUE
    \* (the bug configuration), the router declares the retry a no-op as soon
    \* as the retryable statement on the committed shard reports a duplicate
    \* stmtId.
    SHORT_CIRCUIT_ON_PARTIAL_TXN

ASSUME SHORT_CIRCUIT_ON_PARTIAL_TXN \in BOOLEAN

\* Symbolic identifiers.
Routers   == {"mongos0", "mongos1"}
Shards    == {"shard0", "shard1"}
RetryableShard    == "shard0"   \* Owns the statement with stmtId = 0.
NonRetryableShard == "shard1"   \* Owns the statement with stmtId = -1.
StmtIdRetryable    == 0
StmtIdNonRetryable == -1

\* Per-shard state machine for the original internal transaction.
\* "none"        — the shard has not seen the original txn
\* "prepared"    — the shard has received its participant write and is in
\*                 prepared state (two-phase commit in flight)
\* "committed"   — the shard has committed its participant write, the stmtId
\*                 history has been durably persisted
\* "aborted"     — never reached in this spec but kept for clarity
TxnShardStates == {"none", "prepared", "committed", "aborted"}

\* Per-retry router state machine.
\* "idle"        — has not run anything
\* "running"     — has dispatched its first statement and is waiting for the
\*                 retryable shard to respond
\* "shortCircuited" — gave up on the rest of the internal txn after seeing
\*                    retriedStmtIds come back from the retryable shard
\* "completed"   — successfully ran both statements of the internal txn
\* "blocked"     — saw an in-progress txn on the other shard and refused to
\*                 short-circuit; the spec uses this to model the safe fix
RouterStates == {"idle", "running", "shortCircuited", "completed", "blocked"}

\* Variables.
\* original: state of the original internal txn at each shard.
VARIABLE originalTxn
\* retryTxn: state of the retry internal txn at each shard. Only used to
\*           track whether the retry's write on NonRetryableShard has been
\*           applied — for the retryable shard the retry is short-circuited
\*           and the original txn's commit state is what matters.
VARIABLE retryTxn
\* stmtHistory: per-shard set of stmtIds that have been durably executed.
\*              The check that drives short-circuit (checkStatementExecuted)
\*              reads from this set.
VARIABLE stmtHistory
\* routerState: per-router state machine, see RouterStates.
VARIABLE routerState
\* writeApplied: per-shard, whether the write owned by that shard has been
\*               made durable in the user collection. The RYOW invariant
\*               is phrased over this set.
VARIABLE writeApplied
\* clientObservedSuccess: whether the client has seen ok from a retry; once
\*                        TRUE, every subsequent read MUST see all writes.
VARIABLE clientObservedSuccess
\* clientRead: TRUE once the client has performed the follow-up read; the
\*             read returns the contents of writeApplied at that moment.
VARIABLE clientRead
\* clientReadSawAllWrites: snapshot captured by the read action; used to
\*                         phrase RYOW as a state invariant.
VARIABLE clientReadSawAllWrites

vars == <<originalTxn, retryTxn, stmtHistory, routerState, writeApplied,
          clientObservedSuccess, clientRead, clientReadSawAllWrites>>

\* Initial state: nothing has happened, no writes applied, both routers idle.
Init ==
    /\ originalTxn = [s \in Shards |-> "none"]
    /\ retryTxn = [s \in Shards |-> "none"]
    /\ stmtHistory = [s \in Shards |-> {}]
    /\ routerState = [r \in Routers |-> "idle"]
    /\ writeApplied = [s \in Shards |-> FALSE]
    /\ clientObservedSuccess = FALSE
    /\ clientRead = FALSE
    /\ clientReadSawAllWrites = FALSE

\* Step 1: mongos0 receives the retryable write and dispatches the
\* internal transaction to both shards. Both shards transition from "none"
\* into "prepared" — they have received the participant write but have not
\* yet hit the durable commit phase of two-phase commit.
RouterDispatchOriginal ==
    /\ routerState["mongos0"] = "idle"
    /\ originalTxn = [s \in Shards |-> "none"]
    /\ routerState' = [routerState EXCEPT !["mongos0"] = "running"]
    /\ originalTxn' = [s \in Shards |-> "prepared"]
    /\ UNCHANGED <<retryTxn, stmtHistory, writeApplied,
                   clientObservedSuccess, clientRead, clientReadSawAllWrites>>

\* Step 2: the retryable shard (shard0) commits its participant write. This
\* is the half-committed window from the ticket — shard0 has durably
\* recorded stmtId 0 and applied its write, but shard1 has not.
RetryableShardCommitOriginal ==
    /\ originalTxn[RetryableShard] = "prepared"
    /\ originalTxn' = [originalTxn EXCEPT ![RetryableShard] = "committed"]
    /\ stmtHistory' = [stmtHistory EXCEPT ![RetryableShard] =
                          stmtHistory[RetryableShard] \cup {StmtIdRetryable}]
    /\ writeApplied' = [writeApplied EXCEPT ![RetryableShard] = TRUE]
    /\ UNCHANGED <<retryTxn, routerState, clientObservedSuccess,
                   clientRead, clientReadSawAllWrites>>

\* Step 3: the non-retryable shard commits its participant write. This is
\* the other half of the original two-phase commit. Once it fires, the
\* original txn is fully committed and there is no longer any retry hazard.
NonRetryableShardCommitOriginal ==
    /\ originalTxn[NonRetryableShard] = "prepared"
    /\ originalTxn' = [originalTxn EXCEPT ![NonRetryableShard] = "committed"]
    /\ writeApplied' = [writeApplied EXCEPT ![NonRetryableShard] = TRUE]
    /\ UNCHANGED <<retryTxn, stmtHistory, routerState,
                   clientObservedSuccess, clientRead, clientReadSawAllWrites>>

\* Step 4: the client loses its connection to mongos0 mid-commit and retries
\* against mongos1. mongos1 starts a new internal transaction for the
\* retry, dispatching its first (retryable) statement to shard0.
RouterDispatchRetry ==
    /\ routerState["mongos1"] = "idle"
    /\ routerState["mongos0"] = "running"
    /\ routerState' = [routerState EXCEPT !["mongos1"] = "running"]
    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory, writeApplied,
                   clientObservedSuccess, clientRead, clientReadSawAllWrites>>

\* Step 5: shard0 processes the retry's first statement. Because the
\* original transaction has already committed on shard0, the stmtId 0
\* lookup hits stmtHistory and shard0 responds with retriedStmtIds = [0].
\* mongos1 now has to decide whether to short-circuit the rest of the
\* internal transaction.
\*
\* Two paths:
\*   1) SHORT_CIRCUIT_ON_PARTIAL_TXN = TRUE — the buggy path. mongos1
\*      sees the duplicate stmtId, declares the entire retry a no-op,
\*      and returns ok to the client without ever dispatching the
\*      non-retryable statement to shard1. The non-retryable write on
\*      shard1 may not have committed yet.
\*   2) SHORT_CIRCUIT_ON_PARTIAL_TXN = FALSE — the safe path. mongos1
\*      first checks whether the original internal transaction has fully
\*      committed across all participants. If shard1 has not yet
\*      committed, mongos1 blocks (in production: it sees a
\*      RetryableTransactionInProgress error or waits for the original
\*      txn's commit decision).
RouterHandleRetryableShardResponse ==
    /\ routerState["mongos1"] = "running"
    /\ StmtIdRetryable \in stmtHistory[RetryableShard]
    /\ IF SHORT_CIRCUIT_ON_PARTIAL_TXN
        THEN
            /\ routerState' = [routerState EXCEPT !["mongos1"] = "shortCircuited"]
            /\ clientObservedSuccess' = TRUE
            /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory, writeApplied,
                           clientRead, clientReadSawAllWrites>>
        ELSE
            /\ IF originalTxn[NonRetryableShard] = "committed"
                THEN
                    /\ routerState' = [routerState EXCEPT !["mongos1"] = "shortCircuited"]
                    /\ clientObservedSuccess' = TRUE
                    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory,
                                   writeApplied, clientRead, clientReadSawAllWrites>>
                ELSE
                    /\ routerState' = [routerState EXCEPT !["mongos1"] = "blocked"]
                    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory,
                                   writeApplied, clientObservedSuccess,
                                   clientRead, clientReadSawAllWrites>>

\* Step 6 (safe path only): once the original transaction commits on shard1,
\* the blocked retry unblocks. The non-retryable statement either is
\* re-executed or the retry confirms the original txn fully landed. Either
\* way, the write on shard1 is durable by the time the retry returns ok.
RouterUnblock ==
    /\ routerState["mongos1"] = "blocked"
    /\ originalTxn[NonRetryableShard] = "committed"
    /\ routerState' = [routerState EXCEPT !["mongos1"] = "completed"]
    /\ clientObservedSuccess' = TRUE
    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory, writeApplied,
                   clientRead, clientReadSawAllWrites>>

\* Step 7: client issues a follow-up read in the same session without
\* afterClusterTime >= commitTimestamp. The read sees the current
\* writeApplied snapshot. If the read finds both writes the spec records
\* RYOW success; if any write is missing the spec records a violation.
ClientRead ==
    /\ clientObservedSuccess = TRUE
    /\ clientRead = FALSE
    /\ clientRead' = TRUE
    /\ clientReadSawAllWrites' = (\A s \in Shards : writeApplied[s])
    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory, routerState,
                   writeApplied, clientObservedSuccess>>

\* Convenience action: original txn commits cleanly with no retry. Lets
\* the model exercise the "no failover" trajectory so we know the spec
\* admits non-buggy traces.
RouterDispatchOriginalCompletes ==
    /\ routerState["mongos0"] = "running"
    /\ originalTxn[RetryableShard] = "committed"
    /\ originalTxn[NonRetryableShard] = "committed"
    /\ routerState["mongos1"] = "idle"
    /\ routerState' = [routerState EXCEPT !["mongos0"] = "completed"]
    /\ clientObservedSuccess' = TRUE
    /\ UNCHANGED <<originalTxn, retryTxn, stmtHistory, writeApplied,
                   clientRead, clientReadSawAllWrites>>

Next ==
    \/ RouterDispatchOriginal
    \/ RetryableShardCommitOriginal
    \/ NonRetryableShardCommitOriginal
    \/ RouterDispatchRetry
    \/ RouterHandleRetryableShardResponse
    \/ RouterUnblock
    \/ ClientRead
    \/ RouterDispatchOriginalCompletes
    \/ UNCHANGED vars

Fairness ==
    /\ WF_vars(RouterDispatchOriginal)
    /\ WF_vars(RetryableShardCommitOriginal)
    /\ WF_vars(NonRetryableShardCommitOriginal)
    /\ WF_vars(RouterDispatchRetry)
    /\ WF_vars(RouterHandleRetryableShardResponse)
    /\ WF_vars(RouterUnblock)
    /\ WF_vars(ClientRead)
    /\ WF_vars(RouterDispatchOriginalCompletes)

Spec == Init /\ [][Next]_vars /\ Fairness

----------------------------------------------------------------------------------
\* Type invariant.
TypeOK ==
    /\ originalTxn \in [Shards -> TxnShardStates]
    /\ retryTxn \in [Shards -> TxnShardStates]
    /\ stmtHistory \in [Shards -> SUBSET {StmtIdRetryable, StmtIdNonRetryable}]
    /\ routerState \in [Routers -> RouterStates]
    /\ writeApplied \in [Shards -> BOOLEAN]
    /\ clientObservedSuccess \in BOOLEAN
    /\ clientRead \in BOOLEAN
    /\ clientReadSawAllWrites \in BOOLEAN

\* Core correctness property: if the client has observed a successful
\* retryable-write response in a given session, every subsequent read in
\* that same session must observe every write that was part of the
\* retryable write — regardless of whether the retry short-circuited.
\* Phrased as a state invariant over the snapshot captured at read time.
ReadYourOwnWrites ==
    clientRead => clientReadSawAllWrites

\* Auxiliary safety: the only way to declare client-visible success is
\* through one of the three documented completion paths. Catches accidental
\* state explosions where clientObservedSuccess flips without any router
\* action.
SuccessIsBackedByRouter ==
    clientObservedSuccess =>
        \/ routerState["mongos0"] = "completed"
        \/ routerState["mongos1"] \in {"shortCircuited", "completed"}

\* Liveness: every initiated run eventually reaches a terminal client state.
EventuallyTerminates ==
    <>(clientRead /\ clientObservedSuccess)

==================================================================================
