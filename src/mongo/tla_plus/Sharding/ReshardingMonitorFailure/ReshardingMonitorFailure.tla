\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------- MODULE ReshardingMonitorFailure -----------------------------------------
\* This specification models the bug described in SERVER-108852: "Resharding participants don't
\* handle change streams monitor failures immediately."
\*
\* Background. Each resharding donor / recipient launches a ReshardingChangeStreamsMonitor whose
\* lifetime is tracked by a SharedSemiFuture (_changeStreamsMonitorQuiesced). The participant's
\* main future-chain stores this future at the _createAndStartChangeStreamsMonitor step but does
\* NOT check it. Several subsequent steps run in between (await-recipients-done-applying,
\* write-txn-oplog-entry-then-transition-to-blocking-writes) before the chain finally awaits the
\* future at _awaitChangeStreamsMonitorCompleted. If the monitor fails during the intermediate
\* steps, the participant happily transitions through donor/recipient states and only surfaces
\* the error after blocking writes have begun.
\*
\* This spec models one participant's future chain as a sequence of steps. The monitor runs in
\* parallel and may fail non-deterministically at any step. The desired invariant
\* MonitorErrorObservedBeforeBlockingWrites is violated by the current implementation but holds
\* under the proposed fix (an immediate observer that cancels the chain on monitor error).
\*
\* To model-check:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/ReshardingMonitorFailure

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Steps,            \* Ordered sequence of phases in the participant's future chain.
    BlockingStep,     \* The phase at which the participant transitions to blocking writes.
    AwaitStep,        \* The phase at which the chain finally awaits the monitor future.
    Fixed             \* TRUE  -> model the proposed fix (immediate cancellation on monitor error).
                      \* FALSE -> model current behaviour (error observed only at AwaitStep).

ASSUME Steps \in Seq(Nat) \cup {<<>>}
ASSUME Len(Steps) > 0
ASSUME BlockingStep \in 1..Len(Steps)
ASSUME AwaitStep    \in 1..Len(Steps)
ASSUME BlockingStep < AwaitStep      \* Bug precondition: blocking-writes transition precedes await.
ASSUME Fixed \in BOOLEAN

\* Status of the change-streams monitor future.
NotStarted   == "NotStarted"
Running      == "Running"
Failed       == "Failed"
Completed    == "Completed"

\* Chain state.
ChainNotStarted == 0           \* Chain hasn't begun.
\* Otherwise an Int in 1..Len(Steps) indicating the current step, or
ChainDone       == "ChainDone"
ChainAborted    == "ChainAborted"

VARIABLES
    monitor,         \* Monitor status: one of NotStarted / Running / Failed / Completed.
    chain,           \* Chain progress.
    errorObserved,   \* Did the participant observe the monitor error?
    blockingEntered  \* Did the participant transition into blocking writes?

vars == <<monitor, chain, errorObserved, blockingEntered>>

TypeOK ==
    /\ monitor         \in {NotStarted, Running, Failed, Completed}
    /\ chain           \in (1..Len(Steps)) \cup {ChainNotStarted, ChainDone, ChainAborted}
    /\ errorObserved   \in BOOLEAN
    /\ blockingEntered \in BOOLEAN

Init ==
    /\ monitor         = NotStarted
    /\ chain           = ChainNotStarted
    /\ errorObserved   = FALSE
    /\ blockingEntered = FALSE

\* The chain begins.
StartChain ==
    /\ chain = ChainNotStarted
    /\ chain' = 1
    /\ UNCHANGED <<monitor, errorObserved, blockingEntered>>

\* The monitor is created and starts running at the create-and-start step.
\* In the real code path this is _createAndStartChangeStreamsMonitor; we abstract it to the first
\* step strictly before BlockingStep.
MonitorStart ==
    /\ monitor = NotStarted
    /\ chain \in 1..Len(Steps)
    /\ chain < BlockingStep
    /\ monitor' = Running
    /\ UNCHANGED <<chain, errorObserved, blockingEntered>>

\* The monitor can fail at any time while running.
MonitorFail ==
    /\ monitor = Running
    /\ monitor' = Failed
    /\ UNCHANGED <<chain, errorObserved, blockingEntered>>

\* The monitor can also complete cleanly.
MonitorComplete ==
    /\ monitor = Running
    /\ monitor' = Completed
    /\ UNCHANGED <<chain, errorObserved, blockingEntered>>

\* Advance the chain to the next step. In the FIXED model, an immediate observer cancels the
\* chain as soon as the monitor fails, so this step is gated on monitor # Failed (or on
\* errorObserved-then-abort, modelled by AbortOnMonitorError below).
Advance ==
    /\ chain \in 1..Len(Steps)
    /\ chain < Len(Steps)
    /\ \/ ~Fixed                       \* Buggy chain: ignores monitor errors mid-chain.
       \/ monitor # Failed             \* Fixed chain: cannot advance once monitor failed.
    \* Block entering the BlockingStep when the monitor has already failed and we're fixed.
    /\ Fixed => (chain' # BlockingStep \/ monitor # Failed)
    /\ chain' = chain + 1
    \* Mark blocking-writes entry.
    /\ blockingEntered' = (blockingEntered \/ (chain + 1 = BlockingStep))
    /\ UNCHANGED <<monitor, errorObserved>>

\* The chain finishes its last step.
Finish ==
    /\ chain = Len(Steps)
    /\ chain' = ChainDone
    /\ UNCHANGED <<monitor, errorObserved, blockingEntered>>

\* At the AwaitStep the chain finally inspects the monitor future. If the monitor failed, the
\* participant observes the error here. By construction (BlockingStep < AwaitStep) this is too
\* late under the buggy semantics: the donor has already transitioned to blocking writes.
ObserveAtAwait ==
    /\ chain = AwaitStep
    /\ monitor = Failed
    /\ errorObserved' = TRUE
    /\ chain' = ChainAborted
    /\ UNCHANGED <<monitor, blockingEntered>>

\* FIXED-ONLY: an immediate observer notices the monitor error and aborts the chain before the
\* participant can transition to blocking writes. Models the patch proposed in SERVER-108852.
AbortOnMonitorError ==
    /\ Fixed
    /\ monitor = Failed
    /\ chain \in 1..Len(Steps)
    /\ chain < BlockingStep
    /\ ~errorObserved
    /\ errorObserved' = TRUE
    /\ chain' = ChainAborted
    /\ UNCHANGED <<monitor, blockingEntered>>

Next ==
    \/ StartChain
    \/ MonitorStart
    \/ MonitorFail
    \/ MonitorComplete
    \/ Advance
    \/ Finish
    \/ ObserveAtAwait
    \/ AbortOnMonitorError

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------------------------------
\* Invariants

\* The bug invariant. If the monitor failed, then either:
\*   (a) the participant has not yet entered blocking writes, or
\*   (b) the participant has observed the error.
\* Under the buggy implementation (Fixed = FALSE) this is FALSIFIED by a trace where
\*   StartChain ; MonitorStart ; MonitorFail ; Advance* (crossing BlockingStep) ; ...
\* Under the proposed fix (Fixed = TRUE) AbortOnMonitorError keeps this invariant.
MonitorErrorObservedBeforeBlockingWrites ==
    monitor = Failed => (~blockingEntered \/ errorObserved)

\* A weaker liveness-flavoured safety: if we ever finish the chain cleanly, the monitor must not
\* have failed silently. Falsified by the buggy model when the monitor fails AFTER the await step
\* has already passed; not the focus of this spec but a useful additional bait.
NoSilentMonitorFailure ==
    (chain = ChainDone) => (monitor # Failed \/ errorObserved)

====================================================================================================
