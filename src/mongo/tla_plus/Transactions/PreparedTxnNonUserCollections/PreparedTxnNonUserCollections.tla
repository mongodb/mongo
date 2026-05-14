\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
---------------------- MODULE PreparedTxnNonUserCollections ----------------------------
\* Formal specification accompanying SERVER-115356 ("Handle all non-user collections
\* involved in prepared transactions").
\*
\* Context (from src/mongo/db/repl/transaction_oplog_application.cpp):
\* When a node reclaims a prepared transaction from a precise checkpoint, it iterates
\* `SessionTxnRecord.affectedNamespaces` and re-acquires MODE_IX on each namespace so
\* that two-phase-locking is restored. Prior to SERVER-113729 we only stored user
\* namespaces; SERVER-115356 asks us to audit every *non-user* collection whose write
\* participated in the prepared transaction and ensure that its lock is also reclaimed.
\*
\* Concretely, the following non-user namespaces may be written to under a prepared
\* user transaction:
\*   - config.image_collection             (retryable findAndModify side-table)
\*   - config.system.preimages             (change-stream pre-images for watched colls)
\*   - config.system.change_collection     (serverless change collections)
\*   - config.transactions                 (session catalog row; written on prepare)
\* The model treats these as a parametric set NonUserNamespaces and asserts that the
\* participant graph reclaimed during recovery equals the participant graph held at the
\* original prepare instant (modulo namespaces dropped between prepare and recovery).
\*
\* To run the model-checker:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh PreparedTxnNonUserCollections

EXTENDS Integers, Sequences, FiniteSets, TLC
CONSTANTS
    UserNamespaces,       \* finite set, e.g. {u1, u2}
    NonUserNamespaces,    \* finite set, e.g. {image, preimg, changeColl, sessionCat}
    Txns,                 \* finite set of prepared transactions, e.g. {t1, t2}
    MAX_OPS               \* per-txn upper bound on participating writes

NoNs == "noNamespace"
Prepared == "prepared"
Reclaimed == "reclaimed"
Aborted == "aborted"
Committed == "committed"
NotStarted == "notStarted"

AllNamespaces == UserNamespaces \union NonUserNamespaces

ASSUME Cardinality(UserNamespaces) >= 1
ASSUME Cardinality(NonUserNamespaces) >= 1
ASSUME Cardinality(Txns) >= 1
ASSUME MAX_OPS \in 1..16
ASSUME UserNamespaces \intersect NonUserNamespaces = {}

VARIABLE txnState         \* [Txns -> {notStarted, prepared, reclaimed, aborted, committed}]
VARIABLE preparedSet      \* [Txns -> SUBSET AllNamespaces]
                          \* The full participant set captured by op-observers AT prepare
                          \* time. Includes user + non-user namespaces.
VARIABLE persistedSet     \* [Txns -> SUBSET AllNamespaces]
                          \* The participant set actually serialised onto the
                          \* SessionTxnRecord.affectedNamespaces field that survives a
                          \* precise checkpoint.
VARIABLE reclaimedSet     \* [Txns -> SUBSET AllNamespaces]
                          \* The participant set re-locked on the recovering node when
                          \* _recoverPreparedTransactionFromPreciseCheckpoint runs.
VARIABLE droppedSince     \* [Txns -> SUBSET AllNamespaces]
                          \* Namespaces dropped between prepare and recovery (these must
                          \* not be re-acquired; the lock is irrelevant).
VARIABLE locks            \* [AllNamespaces -> SUBSET Txns]  \* MODE_IX holders.
VARIABLE opsLeft          \* [Txns -> 0..MAX_OPS]            \* state-space cap.

vars == << txnState, preparedSet, persistedSet, reclaimedSet,
           droppedSince, locks, opsLeft >>

(*****************************************************************************)
(* Helpers.                                                                  *)
(*****************************************************************************)

\* Classify a namespace as user-data or system-internal.
IsUser(ns) == ns \in UserNamespaces
IsNonUser(ns) == ns \in NonUserNamespaces

\* The op-observer rule: any write that goes through the retryable
\* findAndModify path implicitly touches config.image_collection; any write to
\* a change-streams-enabled collection implicitly touches config.system.preimages.
\* The spec abstracts that into nondeterministic membership in `preparedSet[t]`.

AllPreparedTxns == { t \in Txns : txnState[t] = Prepared }

(*****************************************************************************)
(* Initial state.                                                            *)
(*****************************************************************************)
Init ==
    /\ txnState     = [t \in Txns |-> NotStarted]
    /\ preparedSet  = [t \in Txns |-> {}]
    /\ persistedSet = [t \in Txns |-> {}]
    /\ reclaimedSet = [t \in Txns |-> {}]
    /\ droppedSince = [t \in Txns |-> {}]
    /\ locks        = [ns \in AllNamespaces |-> {}]
    /\ opsLeft      = [t \in Txns |-> MAX_OPS]

(*****************************************************************************)
(* Transitions.                                                              *)
(*****************************************************************************)

\* A transaction performs a write on `ns`. This both extends the prepared
\* participant graph and takes a MODE_IX lock.
TxnWrite(t, ns) ==
    /\ txnState[t] = NotStarted
    /\ opsLeft[t] > 0
    /\ txnState' = txnState
    /\ preparedSet'  = [preparedSet  EXCEPT ![t] = @ \union {ns}]
    /\ locks'        = [locks        EXCEPT ![ns] = @ \union {t}]
    /\ opsLeft'      = [opsLeft      EXCEPT ![t] = @ - 1]
    /\ UNCHANGED << persistedSet, reclaimedSet, droppedSince >>

\* A user write that implicitly drags in a non-user side-table write.
\* Mirrors the op-observer behaviour for findAndModify (image_collection) and
\* change-streams (system.preimages).
TxnImplicitNonUserWrite(t, userNs, nonUserNs) ==
    /\ txnState[t] = NotStarted
    /\ opsLeft[t] > 0
    /\ userNs \in UserNamespaces
    /\ nonUserNs \in NonUserNamespaces
    /\ txnState' = txnState
    /\ preparedSet' = [preparedSet EXCEPT ![t] = @ \union {userNs, nonUserNs}]
    /\ locks' = [ns \in AllNamespaces |->
                    IF ns \in {userNs, nonUserNs}
                    THEN locks[ns] \union {t}
                    ELSE locks[ns]]
    /\ opsLeft' = [opsLeft EXCEPT ![t] = @ - 1]
    /\ UNCHANGED << persistedSet, reclaimedSet, droppedSince >>

\* PrepareTransaction: persist the affectedNamespaces field onto the session
\* record. The bug surface for SERVER-115356 is exactly the membership of
\* `persistedSet[t]` w.r.t. `preparedSet[t]`.
\*
\* Mode A ("complete persistence", post-fix): every participant -- user AND
\*   non-user -- is written to the session record.
\* Mode B ("incomplete persistence", pre-fix): non-user participants are
\*   silently dropped. Enabled when the InjectIncompletePersistence operator
\*   is true; the invariant ParticipantGraphClosed catches the resulting bug.
\*
\* Mode is chosen non-deterministically per-txn so TLC explores both.

PreparePersistComplete(t) ==
    /\ txnState[t] = NotStarted
    /\ preparedSet[t] # {}
    /\ txnState' = [txnState EXCEPT ![t] = Prepared]
    /\ persistedSet' = [persistedSet EXCEPT ![t] = preparedSet[t]]
    /\ UNCHANGED << preparedSet, reclaimedSet, droppedSince, locks, opsLeft >>

PreparePersistOnlyUser(t) ==
    \* Models the pre-SERVER-115356 behaviour. Keep ALSO covered in the model
    \* so that the invariant catches a regression.
    /\ txnState[t] = NotStarted
    /\ preparedSet[t] # {}
    /\ txnState' = [txnState EXCEPT ![t] = Prepared]
    /\ persistedSet' = [persistedSet EXCEPT ![t] =
                           { ns \in preparedSet[t] : IsUser(ns) }]
    /\ UNCHANGED << preparedSet, reclaimedSet, droppedSince, locks, opsLeft >>

\* A namespace gets dropped between prepare and recovery. Locks held by
\* prepared transactions remain logically associated with the txn even though
\* the namespace no longer exists; recovery is expected to skip it.
DropNamespaceBetweenPrepareAndRecovery(t, ns) ==
    /\ txnState[t] = Prepared
    /\ ns \in preparedSet[t]
    /\ ns \notin droppedSince[t]
    /\ droppedSince' = [droppedSince EXCEPT ![t] = @ \union {ns}]
    /\ UNCHANGED << txnState, preparedSet, persistedSet, reclaimedSet, locks,
                    opsLeft >>

\* Crash + reboot: locks are wiped, transaction state moves from Prepared to
\* Reclaimed, and we re-acquire MODE_IX on exactly the namespaces that survived
\* in persistedSet AND were not dropped in the meantime.
RecoverFromPreciseCheckpoint(t) ==
    /\ txnState[t] = Prepared
    /\ LET nsSurvive == persistedSet[t] \ droppedSince[t]
       IN /\ reclaimedSet' = [reclaimedSet EXCEPT ![t] = nsSurvive]
          /\ locks' = [ns \in AllNamespaces |->
                          IF ns \in nsSurvive
                          THEN locks[ns] \union {t}
                          ELSE locks[ns] \ {t}]
    /\ txnState' = [txnState EXCEPT ![t] = Reclaimed]
    /\ UNCHANGED << preparedSet, persistedSet, droppedSince, opsLeft >>

\* Once reclaimed, the transaction can be aborted or committed.
CommitReclaimed(t) ==
    /\ txnState[t] = Reclaimed
    /\ txnState' = [txnState EXCEPT ![t] = Committed]
    /\ locks' = [ns \in AllNamespaces |-> locks[ns] \ {t}]
    /\ UNCHANGED << preparedSet, persistedSet, reclaimedSet, droppedSince,
                    opsLeft >>

AbortReclaimed(t) ==
    /\ txnState[t] = Reclaimed
    /\ txnState' = [txnState EXCEPT ![t] = Aborted]
    /\ locks' = [ns \in AllNamespaces |-> locks[ns] \ {t}]
    /\ UNCHANGED << preparedSet, persistedSet, reclaimedSet, droppedSince,
                    opsLeft >>

Next ==
    \/ \E t \in Txns, ns \in AllNamespaces : TxnWrite(t, ns)
    \/ \E t \in Txns, u \in UserNamespaces, n \in NonUserNamespaces :
           TxnImplicitNonUserWrite(t, u, n)
    \/ \E t \in Txns : PreparePersistComplete(t)
    \/ \E t \in Txns : PreparePersistOnlyUser(t)
    \/ \E t \in Txns, ns \in AllNamespaces :
           DropNamespaceBetweenPrepareAndRecovery(t, ns)
    \/ \E t \in Txns : RecoverFromPreciseCheckpoint(t)
    \/ \E t \in Txns : CommitReclaimed(t)
    \/ \E t \in Txns : AbortReclaimed(t)

Spec == Init /\ [][Next]_vars

(*****************************************************************************)
(* Type invariant.                                                           *)
(*****************************************************************************)
TypeOK ==
    /\ txnState     \in [Txns -> {NotStarted, Prepared, Reclaimed, Aborted, Committed}]
    /\ preparedSet  \in [Txns -> SUBSET AllNamespaces]
    /\ persistedSet \in [Txns -> SUBSET AllNamespaces]
    /\ reclaimedSet \in [Txns -> SUBSET AllNamespaces]
    /\ droppedSince \in [Txns -> SUBSET AllNamespaces]
    /\ locks        \in [AllNamespaces -> SUBSET Txns]
    /\ opsLeft      \in [Txns -> 0..MAX_OPS]

(*****************************************************************************)
(* Core correctness invariants.                                              *)
(*****************************************************************************)

\* The participant graph that survives a precise checkpoint must cover every
\* namespace -- user AND non-user -- that the transaction wrote to before
\* prepare. This is the SERVER-115356 contract: BaitParticipantGraphIncomplete
\* below is the negation, used to fish out the pre-fix counter-example.
ParticipantGraphClosed ==
    \A t \in Txns :
        txnState[t] \in {Prepared, Reclaimed, Aborted, Committed} =>
            preparedSet[t] \subseteq persistedSet[t]

\* After recovery from a precise checkpoint, the reclaimed lock set covers
\* every still-existing participant. Stronger than necessary if
\* ParticipantGraphClosed holds; kept independent so an isolated regression
\* in the recovery path is caught.
RecoveredLocksCoverAllParticipants ==
    \A t \in Txns :
        txnState[t] = Reclaimed =>
            (preparedSet[t] \ droppedSince[t]) \subseteq reclaimedSet[t]

\* Two-phase locking: while in Prepared or Reclaimed state, the txn holds
\* MODE_IX on every participating namespace that still exists.
TwoPhaseLockingHeld ==
    \A t \in Txns :
        txnState[t] = Reclaimed =>
            \A ns \in reclaimedSet[t] : t \in locks[ns]

\* Non-user collections must not be silently amnesia'd by the persistence
\* layer. Cataloguing the property explicitly makes the diff against the
\* prior (user-only) behaviour visible in the model.
NonUserNamespacesPersisted ==
    \A t \in Txns :
        txnState[t] \in {Prepared, Reclaimed, Aborted, Committed} =>
            { ns \in preparedSet[t] : IsNonUser(ns) } \subseteq persistedSet[t]

\* A prepared transaction that wrote to image_collection / preimages MUST
\* re-acquire those locks on recovery. This is the specific case SERVER-115356
\* calls out.
RecoveryReclaimsNonUserLocks ==
    \A t \in Txns :
        txnState[t] = Reclaimed =>
            \A ns \in preparedSet[t] :
                IsNonUser(ns) /\ ns \notin droppedSince[t] => t \in locks[ns]

(*****************************************************************************)
(* Bait invariants. Each is a temporary `=>` negation used to fish a TLC     *)
(* counter-example. They MUST stay disabled in MCPreparedTxnNonUserCollections*)
(* unless we are debugging.                                                  *)
(*****************************************************************************)
BaitParticipantGraphIncomplete ==
    ~ \E t \in Txns :
        /\ txnState[t] = Prepared
        /\ \E ns \in NonUserNamespaces :
              ns \in preparedSet[t] /\ ns \notin persistedSet[t]

BaitRecoveryDroppedNonUserLock ==
    ~ \E t \in Txns, ns \in NonUserNamespaces :
        /\ txnState[t] = Reclaimed
        /\ ns \in preparedSet[t]
        /\ ns \notin droppedSince[t]
        /\ t \notin locks[ns]

====================================================================================================
