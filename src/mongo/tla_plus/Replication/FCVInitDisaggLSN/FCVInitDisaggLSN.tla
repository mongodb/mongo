\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE FCVInitDisaggLSN ---------------------------
\* Sibling spec to RaftMongoReplTimestamp, scoped to the FCV-init handshake on
\* a node backed by disaggregated storage. The bug it fingers is the temporary
\* window during FCV-init in which the log-service LSN (disaggregated tier) and
\* the local stable timestamp (recoverable tier) diverge, yet the init code
\* path returns success to the FCV-set command. A later restart or stepdown
\* observed against the local stable timestamp can then reveal state the log
\* service believes has not yet been emitted, or vice-versa - either is a
\* bootstrap-coherence violation.
\*
\* The spec deliberately abstracts the replication protocol (covered in the
\* sibling RaftMongoReplTimestamp.tla) and concentrates on the FCV-init state
\* machine plus the two LSN streams it has to reconcile.
\*
\* To run the model-checker, edit MCFCVInitDisaggLSN.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/FCVInitDisaggLSN

EXTENDS Integers, FiniteSets, Sequences, TLC

\* Set of replica-set nodes participating in FCV init. Single-node configs
\* still exercise the handshake; multi-node configs exercise the cross-node
\* propagation of the disagg LSN as well.
CONSTANT Server

\* Upper bound on how many distinct write events the local writer emits during
\* one model-checking behavior. Each write bumps the local timestamp by 1.
CONSTANT MaxLocalWrites

\* Upper bound on how many distinct flushes the log-service receives. Each
\* flush bumps the log-service LSN by 1. Decoupled from MaxLocalWrites because
\* the disagg tier batches.
CONSTANT MaxLogServiceFlushes

\* Bug toggle. When TRUE, the FCV-init state machine is allowed to transition
\* to Returned even though logServiceLSN and localStableTS have not yet met.
\* Setting this FALSE models the proposed fix - init blocks in WaitConvergence
\* until the two streams agree.
CONSTANT AllowReturnBeforeConvergence

----
\* Per-server state.

\* FCV init phase for each server.
\*   Idle           - the server has not yet begun init.
\*   LoadFCVDoc     - the on-disk FCV document is being loaded; localStableTS
\*                    is being primed from the recovery oplog.
\*   OpenLogService - the disaggregated log-service handle is being opened and
\*                    the log-service LSN is being primed.
\*   WaitConvergence - both streams are being driven forward; init blocks
\*                     waiting for them to meet.
\*   Returned       - init has returned success to the caller. Steady state.
VARIABLE fcvPhase

\* The local recovery stable timestamp on each server. Monotone non-decreasing.
VARIABLE localStableTS

\* The log-service LSN seen by each server. Monotone non-decreasing. In the
\* steady state this must equal localStableTS.
VARIABLE logServiceLSN

\* The commit LSN of the FCV document itself - the smallest watermark that
\* init must clear before it is safe to return. Set once during LoadFCVDoc
\* and immutable thereafter for the lifetime of one init.
VARIABLE fcvDocCommitLSN

\* How many times each server has been observed in the Returned phase with a
\* coherent (logServiceLSN = localStableTS) pair. Used by liveness checking.
VARIABLE convergedReturns

serverVars == <<fcvPhase, localStableTS, logServiceLSN, fcvDocCommitLSN,
                convergedReturns>>

\* Trace of init-return events. Each element is a record
\*   [server |-> s, localStableTS |-> n, logServiceLSN |-> m, fcvDocCommitLSN |-> k]
\* captured at the instant the server transitioned into Returned. We keep
\* this as a set, not a sequence, so model-checking does not blow up on
\* permutation orderings of independent returns.
VARIABLE returnTrace

vars == <<serverVars, returnTrace>>

----
\* Helpers.

\* Phase ordering helper. Returns TRUE when phase a is strictly earlier than
\* phase b in the documented FCV-init progression.
PhaseRank(p) ==
    IF p = "Idle"           THEN 0
    ELSE IF p = "LoadFCVDoc"     THEN 1
    ELSE IF p = "OpenLogService" THEN 2
    ELSE IF p = "WaitConvergence" THEN 3
    ELSE 4

PhaseLT(a, b) == PhaseRank(a) < PhaseRank(b)

\* All servers have reached the steady state.
AllReturned == \A s \in Server : fcvPhase[s] = "Returned"

----
\* Initial state. Every server starts Idle with zero on every LSN.
Init ==
    /\ fcvPhase         = [s \in Server |-> "Idle"]
    /\ localStableTS    = [s \in Server |-> 0]
    /\ logServiceLSN    = [s \in Server |-> 0]
    /\ fcvDocCommitLSN  = [s \in Server |-> 0]
    /\ convergedReturns = [s \in Server |-> 0]
    /\ returnTrace      = {}

----
\* Actions.

\* Begin FCV init on server s. Idle -> LoadFCVDoc.
BeginInit(s) ==
    /\ fcvPhase[s] = "Idle"
    /\ fcvPhase' = [fcvPhase EXCEPT ![s] = "LoadFCVDoc"]
    /\ UNCHANGED <<localStableTS, logServiceLSN, fcvDocCommitLSN,
                   convergedReturns, returnTrace>>

\* Server s loads the FCV doc. The doc's commit LSN is pinned. The local
\* stable timestamp is primed to the same watermark. The log-service stream
\* has not yet been touched.
LoadFCVDoc(s) ==
    /\ fcvPhase[s] = "LoadFCVDoc"
    /\ \E w \in 1..MaxLocalWrites :
        /\ w > localStableTS[s]
        /\ localStableTS' = [localStableTS EXCEPT ![s] = w]
        /\ fcvDocCommitLSN' = [fcvDocCommitLSN EXCEPT ![s] = w]
    /\ fcvPhase' = [fcvPhase EXCEPT ![s] = "OpenLogService"]
    /\ UNCHANGED <<logServiceLSN, convergedReturns, returnTrace>>

\* Server s opens its handle to the disaggregated log service. The handle
\* exposes a starting LSN that lags fcvDocCommitLSN by some amount in [0, F]
\* where F is the configured flush budget. This is exactly the window the
\* bug fingers.
OpenLogService(s) ==
    /\ fcvPhase[s] = "OpenLogService"
    /\ \E l \in 0..MaxLogServiceFlushes :
        /\ logServiceLSN' = [logServiceLSN EXCEPT ![s] = l]
    /\ fcvPhase' = [fcvPhase EXCEPT ![s] = "WaitConvergence"]
    /\ UNCHANGED <<localStableTS, fcvDocCommitLSN, convergedReturns,
                   returnTrace>>

\* Background driver: a flush lands at the log service, bumping its LSN
\* toward localStableTS. Monotone, only enabled in WaitConvergence - this
\* is the driver the FCV-init loop is waiting on. Post-Returned the two
\* streams move together (see SteadyStateWrite).
AdvanceLogServiceLSN(s) ==
    /\ fcvPhase[s] = "WaitConvergence"
    /\ logServiceLSN[s] < localStableTS[s]
    /\ logServiceLSN' = [logServiceLSN EXCEPT ![s] = logServiceLSN[s] + 1]
    /\ UNCHANGED <<fcvPhase, localStableTS, fcvDocCommitLSN,
                   convergedReturns, returnTrace>>

\* Background driver: a local write bumps the local stable timestamp while
\* init is still sleeping in WaitConvergence. Models incidental writes
\* (e.g. oplog application, internal collections) that the disagg tier
\* will subsequently catch up to via AdvanceLogServiceLSN.
AdvanceLocalStableTS(s) ==
    /\ fcvPhase[s] = "WaitConvergence"
    /\ localStableTS[s] < MaxLocalWrites
    /\ localStableTS' = [localStableTS EXCEPT ![s] = localStableTS[s] + 1]
    /\ UNCHANGED <<fcvPhase, logServiceLSN, fcvDocCommitLSN,
                   convergedReturns, returnTrace>>

\* Steady-state write after init returns. Both streams advance together;
\* this is the post-init invariant the disagg tier enforces via durable
\* journal commit ordering. Modelled as a single atomic step so the
\* coherent-Returned invariant is preserved.
SteadyStateWrite(s) ==
    /\ fcvPhase[s] = "Returned"
    /\ localStableTS[s] < MaxLocalWrites
    /\ localStableTS[s] = logServiceLSN[s]
    /\ localStableTS' = [localStableTS EXCEPT ![s] = localStableTS[s] + 1]
    /\ logServiceLSN' = [logServiceLSN EXCEPT ![s] = logServiceLSN[s] + 1]
    /\ UNCHANGED <<fcvPhase, fcvDocCommitLSN, convergedReturns,
                   returnTrace>>

\* Convergent return. WaitConvergence -> Returned, only when the two streams
\* agree and both are at or above the FCV-doc commit LSN. This is the path
\* the proposed fix takes.
ReturnConverged(s) ==
    /\ fcvPhase[s] = "WaitConvergence"
    /\ logServiceLSN[s] = localStableTS[s]
    /\ logServiceLSN[s] >= fcvDocCommitLSN[s]
    /\ fcvPhase' = [fcvPhase EXCEPT ![s] = "Returned"]
    /\ convergedReturns' = [convergedReturns EXCEPT ![s] =
                                convergedReturns[s] + 1]
    /\ returnTrace' = returnTrace \union {[
                          server          |-> s,
                          localStableTS   |-> localStableTS[s],
                          logServiceLSN   |-> logServiceLSN[s],
                          fcvDocCommitLSN |-> fcvDocCommitLSN[s]]}
    /\ UNCHANGED <<localStableTS, logServiceLSN, fcvDocCommitLSN>>

\* Buggy return. WaitConvergence -> Returned without confirming the two
\* streams agree. Only enabled when AllowReturnBeforeConvergence is TRUE,
\* which is exactly the toggle SERVER-112445 flipped on by accident.
ReturnEarly(s) ==
    /\ AllowReturnBeforeConvergence
    /\ fcvPhase[s] = "WaitConvergence"
    /\ logServiceLSN[s] # localStableTS[s]
    /\ fcvPhase' = [fcvPhase EXCEPT ![s] = "Returned"]
    /\ returnTrace' = returnTrace \union {[
                          server          |-> s,
                          localStableTS   |-> localStableTS[s],
                          logServiceLSN   |-> logServiceLSN[s],
                          fcvDocCommitLSN |-> fcvDocCommitLSN[s]]}
    /\ UNCHANGED <<localStableTS, logServiceLSN, fcvDocCommitLSN,
                   convergedReturns>>

----
\* Action labels - one disjunct per per-server primitive.

BeginInitAction         == \E s \in Server : BeginInit(s)
LoadFCVDocAction        == \E s \in Server : LoadFCVDoc(s)
OpenLogServiceAction    == \E s \in Server : OpenLogService(s)
AdvanceLogServiceLSNAction == \E s \in Server : AdvanceLogServiceLSN(s)
AdvanceLocalStableTSAction == \E s \in Server : AdvanceLocalStableTS(s)
SteadyStateWriteAction  == \E s \in Server : SteadyStateWrite(s)
ReturnConvergedAction   == \E s \in Server : ReturnConverged(s)
ReturnEarlyAction       == \E s \in Server : ReturnEarly(s)

Next ==
    \/ BeginInitAction
    \/ LoadFCVDocAction
    \/ OpenLogServiceAction
    \/ AdvanceLogServiceLSNAction
    \/ AdvanceLocalStableTSAction
    \/ SteadyStateWriteAction
    \/ ReturnConvergedAction
    \/ ReturnEarlyAction

SpecBehavior == Init /\ [][Next]_vars

\* Fairness. The drivers and the convergent return must be live - otherwise
\* AllReturned is unreachable and any liveness property is vacuous.
Liveness ==
    /\ WF_vars(BeginInitAction)
    /\ WF_vars(LoadFCVDocAction)
    /\ WF_vars(OpenLogServiceAction)
    /\ SF_vars(AdvanceLogServiceLSNAction)
    /\ WF_vars(ReturnConvergedAction)

Spec == SpecBehavior /\ Liveness

----
\* Invariants.

\* The headline invariant. Every server that has Returned exposes a coherent
\* pair: the two streams agree and both have cleared the FCV-doc commit LSN.
\* In the green model this is impossible to violate. With the bug toggle on
\* (ReturnEarly enabled) TLC produces a counterexample reaching Returned
\* with logServiceLSN # localStableTS.
BootstrapCoherence ==
    \A s \in Server :
        fcvPhase[s] = "Returned" =>
            /\ logServiceLSN[s] = localStableTS[s]
            /\ logServiceLSN[s] >= fcvDocCommitLSN[s]
            /\ localStableTS[s] >= fcvDocCommitLSN[s]

\* Every record persisted into the return trace must itself be coherent. A
\* trace entry is the public face of init success - a debugger, a startup
\* log, or a downstream stepdown handler can observe it - so the same
\* coherence rule applies row-for-row.
ReturnTraceCoherence ==
    \A r \in returnTrace :
        /\ r.logServiceLSN = r.localStableTS
        /\ r.logServiceLSN >= r.fcvDocCommitLSN

\* Monotonicity. Neither LSN may regress.
\* (TLC will produce a counterexample if a future action were ever to
\* decrement either, which is one of the easier ways to introduce this bug
\* by accident in the C++ when refactoring.)
LSNsMonotone ==
    [][/\ \A s \in Server : localStableTS'[s] >= localStableTS[s]
       /\ \A s \in Server : logServiceLSN'[s] >= logServiceLSN[s]]_vars

\* No phase may regress.
PhaseMonotone ==
    [][\A s \in Server :
         PhaseRank(fcvPhase'[s]) >= PhaseRank(fcvPhase[s])]_vars

\* FCV-doc commit LSN is set once and then frozen. Any later mutation is a
\* state-machine violation.
FCVDocCommitFrozen ==
    [][\A s \in Server :
         fcvDocCommitLSN[s] # 0 => fcvDocCommitLSN'[s] = fcvDocCommitLSN[s]]_vars

\* A coherent Returned state cannot regress - convergedReturns is monotone.
\* This catches accidental re-entry of WaitConvergence after a coherent
\* return, which would silently widen the bug window on subsequent calls.
ConvergedReturnsMonotone ==
    [][\A s \in Server :
         convergedReturns'[s] >= convergedReturns[s]]_vars

----
\* Liveness.

\* Every server that begins init eventually returns coherently. Checked only
\* in the green model (AllowReturnBeforeConvergence = FALSE); with the bug
\* toggle on we want to see the safety counterexample first, and liveness
\* is dominated by the safety violation.
AllServersEventuallyReturn ==
    \A s \in Server : <>(fcvPhase[s] = "Returned")

\* And when they return, they return coherently. Stronger than the safety
\* invariant because it also rules out indefinite stuttering in
\* WaitConvergence.
EventuallyCoherent ==
    \A s \in Server :
        <>(/\ fcvPhase[s] = "Returned"
           /\ logServiceLSN[s] = localStableTS[s]
           /\ logServiceLSN[s] >= fcvDocCommitLSN[s])

===============================================================================
