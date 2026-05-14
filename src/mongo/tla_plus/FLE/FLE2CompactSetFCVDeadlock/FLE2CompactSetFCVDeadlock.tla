\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

---------------------- MODULE FLE2CompactSetFCVDeadlock ------------------------
\* Models the lock-acquisition resource graph between the FLE2 compact/cleanup
\* command thread and a concurrent setFeatureCompatibilityVersion (setFCV)
\* thread, and checks for a deadlock-freedom invariant on the wait-for graph.
\*
\* Background (see SERVER-122159):
\*
\* The FLE2 compact/cleanup command path (fle2_cleanup_cmd.cpp / fle2_compact_cmd.cpp)
\* first acquires the global lock in IX mode via AutoGetDb on the encrypted data
\* collection's database, then later starts an internal transaction which has to
\* acquire the MultiDocumentTransactionsBarrier (MDTB) lock in IX mode before
\* proceeding.
\*
\* The setFCV command path (set_feature_compatibility_version_command.cpp) takes
\* the global lock in S mode, which under d_concurrency.cpp requires first taking
\* the MultiDocumentTransactionsBarrier lock in S mode, then taking the global
\* lock (S) afterwards.
\*
\* Conflict matrix between the two threads' resource requests:
\*   - Global lock: IX (Compact) vs S (setFCV) -- incompatible.
\*   - MDTB lock:   IX (Compact) vs S (setFCV) -- incompatible.
\*
\* A FixedFCVRegion is created at the beginning of the FLE2 compact/cleanup
\* commands which was intended to make this race impossible by blocking setFCV
\* once compact has started. The bug is that the FixedFCVRegion no longer
\* provides that guarantee, so the following interleaving is reachable:
\*
\*   1. setFCV  acquires MDTB in S  (still trying to take Global S).
\*   2. Compact acquires Global IX  (via AutoGetDb).
\*   3. Compact tries to acquire MDTB IX (for the internal txn). Blocks on
\*      setFCV's MDTB-S holder.
\*   4. setFCV  tries to acquire Global S. Blocks on Compact's Global-IX holder.
\*
\* The wait-for graph then has the cycle
\*     setFCV --(waits MDTB)-> Compact --(waits Global)-> setFCV
\* which is the deadlock.
\*
\* The fix (per the bug report) is to enforce a consistent acquisition order:
\* either Compact must acquire MDTB before Global, or setFCV must be blocked
\* by an outer barrier (e.g. a refreshed FixedFCVRegion) before compact can
\* start grabbing Global. This spec models the bug as a configurable
\* `CompactAcquiresInOrder' flag so that the deadlock-freedom invariant fails
\* with the bug enabled and passes with the fix enabled.
\*
\* To model-check this spec:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh FLE/FLE2CompactSetFCVDeadlock

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    \* Whether the FLE2 compact/cleanup thread acquires its locks in the
    \* deadlock-safe order (MDTB before Global). When TRUE, the wait-for graph
    \* can never form a cycle (fixed behaviour). When FALSE, the buggy
    \* interleaving from SERVER-122159 is reachable.
    CompactAcquiresInOrder

\* Threads.
Compact == "Compact"
SetFCV  == "SetFCV"
Threads == {Compact, SetFCV}

\* Lock resources.
GlobalLock == "Global"
MDTBLock   == "MDTB"
Resources  == {GlobalLock, MDTBLock}

\* Lock modes -- only the modes that appear in this race are modelled.
\*   IX is held by the FLE2 internal-txn client thread.
\*   S  is held by the setFCV thread.
Modes == {"IX", "S"}

\* IX and S are mutually incompatible. (See MongoDB's lock compatibility matrix.)
\* S-S and IX-IX are both compatible, so only the mixed pairs return TRUE.
Conflicts(m1, m2) ==
    \/ (m1 = "IX" /\ m2 = "S")
    \/ (m1 = "S"  /\ m2 = "IX")

\* Mode requested by thread t on resource r per the production code.
RequestedMode(t, r) ==
    CASE t = Compact /\ r = GlobalLock -> "IX"
      [] t = Compact /\ r = MDTBLock   -> "IX"
      [] t = SetFCV  /\ r = GlobalLock -> "S"
      [] t = SetFCV  /\ r = MDTBLock   -> "S"

\* Per-thread program-counter states.
\*   "idle"        : thread has not started.
\*   "want_<R>"    : thread is about to request resource R.
\*   "blocked_<R>" : thread is waiting for resource R.
\*   "done"        : thread released its locks and completed.
PCStates == {"idle",
             "want_MDTB", "blocked_MDTB",
             "want_Global", "blocked_Global",
             "done"}

VARIABLES
    \* heldBy[r] is a set of <<thread, mode>> records currently holding r.
    heldBy,
    \* pc[t] is the program-counter state of thread t.
    pc,
    \* held[t] is the set of resources thread t currently holds (for cleanup).
    held

vars == <<heldBy, pc, held>>

-------------------------------------------------------------------------------
\* Helpers.
-------------------------------------------------------------------------------

\* TRUE iff there is some holder of r whose mode conflicts with mode m.
SomeHolderConflicts(r, m) ==
    \E h \in heldBy[r] : Conflicts(h.mode, m)

\* The acquisition order each thread follows.
\*   - SetFCV always goes MDTB-first then Global (matches d_concurrency.cpp).
\*   - Compact's order depends on CompactAcquiresInOrder:
\*       * TRUE  (fixed)  : MDTB first, then Global -- same order as SetFCV.
\*       * FALSE (buggy)  : Global first (via AutoGetDb), then MDTB
\*                          (via internal txn).
\*
\* Steps returned as a sequence of resources to acquire.
Order(t) ==
    CASE t = SetFCV                            -> <<MDTBLock, GlobalLock>>
      [] t = Compact /\ CompactAcquiresInOrder -> <<MDTBLock, GlobalLock>>
      [] t = Compact /\ ~CompactAcquiresInOrder -> <<GlobalLock, MDTBLock>>

\* The "want_..." state for resource r.
WantPC(r)    == IF r = MDTBLock THEN "want_MDTB"    ELSE "want_Global"
\* The "blocked_..." state for resource r.
BlockedPC(r) == IF r = MDTBLock THEN "blocked_MDTB" ELSE "blocked_Global"

\* TRUE iff thread t is currently waiting (blocked) on resource r.
WaitingOn(t, r) ==
    pc[t] = BlockedPC(r)

\* The wait-for graph: t1 -> t2 iff t1 is waiting on a resource currently held
\* in a conflicting mode by t2.
WaitsFor(t1, t2) ==
    /\ t1 # t2
    /\ \E r \in Resources :
        /\ WaitingOn(t1, r)
        /\ \E h \in heldBy[r] :
            /\ h.thread = t2
            /\ Conflicts(h.mode, RequestedMode(t1, r))

\* TRUE iff the wait-for graph contains a 2-cycle.
\* We only have two threads, so a 2-cycle is the only possible cycle.
HasCycle2 ==
    \E t1, t2 \in Threads :
        /\ t1 # t2
        /\ WaitsFor(t1, t2)
        /\ WaitsFor(t2, t1)

-------------------------------------------------------------------------------
\* Initial state and transitions.
-------------------------------------------------------------------------------

Init ==
    /\ heldBy = [r \in Resources |-> {}]
    /\ pc    = [t \in Threads   |-> "idle"]
    /\ held  = [t \in Threads   |-> {}]

\* Thread t starts up and announces it wants its first resource.
Start(t) ==
    /\ pc[t] = "idle"
    /\ LET firstResource == Order(t)[1] IN
        pc' = [pc EXCEPT ![t] = WantPC(firstResource)]
    /\ UNCHANGED <<heldBy, held>>

\* Thread t tries to acquire resource r in its requested mode.
\* Successful path: no conflict -> grant -> advance pc to "want_<next>"
\*                  or "done" if r was the last resource in t's order.
TryAcquire(t, r) ==
    /\ pc[t] = WantPC(r)
    /\ ~SomeHolderConflicts(r, RequestedMode(t, r))
    /\ heldBy' = [heldBy EXCEPT ![r] = @ \union
                    {[thread |-> t, mode |-> RequestedMode(t, r)]}]
    /\ held'   = [held   EXCEPT ![t] = @ \union {r}]
    /\ LET ord    == Order(t)
           idx    == CHOOSE i \in 1..Len(ord) : ord[i] = r
           nextPC == IF idx = Len(ord)
                     THEN "done"
                     ELSE WantPC(ord[idx + 1])
       IN  pc' = [pc EXCEPT ![t] = nextPC]

\* Thread t requests resource r, finds a conflicting holder, and blocks.
BlockOnAcquire(t, r) ==
    /\ pc[t] = WantPC(r)
    /\ SomeHolderConflicts(r, RequestedMode(t, r))
    /\ pc' = [pc EXCEPT ![t] = BlockedPC(r)]
    /\ UNCHANGED <<heldBy, held>>

\* A blocked thread t becomes unblocked when no holder conflicts with its
\* requested mode anymore. It transitions back to its "want_<R>" state, from
\* which TryAcquire will fire next.
Unblock(t, r) ==
    /\ pc[t] = BlockedPC(r)
    /\ ~SomeHolderConflicts(r, RequestedMode(t, r))
    /\ pc' = [pc EXCEPT ![t] = WantPC(r)]
    /\ UNCHANGED <<heldBy, held>>

\* A done thread releases all its held resources. This models the end of the
\* command -- not a partial unlock. Compact / setFCV both release their locks
\* together when the command finishes.
Release(t) ==
    /\ pc[t] = "done"
    /\ held[t] # {}
    /\ heldBy' = [r \in Resources |->
                    {h \in heldBy[r] : h.thread # t}]
    /\ held'   = [held EXCEPT ![t] = {}]
    /\ UNCHANGED pc

Next ==
    \/ \E t \in Threads : Start(t)
    \/ \E t \in Threads : \E r \in Resources : TryAcquire(t, r)
    \/ \E t \in Threads : \E r \in Resources : BlockOnAcquire(t, r)
    \/ \E t \in Threads : \E r \in Resources : Unblock(t, r)
    \/ \E t \in Threads : Release(t)

Spec == /\ Init
        /\ [][Next]_vars
        /\ WF_vars(Next)

-------------------------------------------------------------------------------
\* Invariants.
-------------------------------------------------------------------------------

TypeOK ==
    /\ pc \in [Threads -> PCStates]
    /\ held \in [Threads -> SUBSET Resources]
    /\ \A r \in Resources :
        \A h \in heldBy[r] :
            /\ h.thread \in Threads
            /\ h.mode \in Modes

\* Lock-manager invariant: no two holders of the same resource hold modes that
\* conflict with each other.
LockManagerConsistent ==
    \A r \in Resources :
        \A h1, h2 \in heldBy[r] :
            (h1 # h2) => ~Conflicts(h1.mode, h2.mode)

\* The held-set on each thread always matches heldBy's per-resource records.
HeldSetConsistent ==
    \A t \in Threads :
        \A r \in Resources :
            (r \in held[t]) <=> (\E h \in heldBy[r] : h.thread = t)

\* Deadlock-freedom: the wait-for graph never has a cycle.
\* With CompactAcquiresInOrder = TRUE  : invariant should hold.
\* With CompactAcquiresInOrder = FALSE : TLC will produce the SERVER-122159
\* counter-example trace.
DeadlockFree == ~HasCycle2

\* The trio of useful invariants.
Invariants ==
    /\ TypeOK
    /\ LockManagerConsistent
    /\ HeldSetConsistent
    /\ DeadlockFree

-------------------------------------------------------------------------------
\* Liveness.
-------------------------------------------------------------------------------

\* In a deadlock-free run, every thread that starts eventually reaches "done"
\* and then releases. Used only with CompactAcquiresInOrder = TRUE.
EventuallyAllDone ==
    \A t \in Threads : (pc[t] # "idle") ~> (pc[t] = "done")

================================================================================
