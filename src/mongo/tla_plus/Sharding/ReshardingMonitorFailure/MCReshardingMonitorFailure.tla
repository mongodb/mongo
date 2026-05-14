---------------------- MODULE MCReshardingMonitorFailure ------------------------------------------
\* Model-checking harness for ReshardingMonitorFailure.tla.
\*
\* We pick a small but representative future chain that mirrors the donor's actual sequence:
\*   1: createAndStartChangeStreamsMonitor
\*   2: awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites
\*   3: writeTxnOplogEntryThenTransitionToBlockingWrites       <- BlockingStep
\*   4: awaitChangeStreamsMonitorCompleted                     <- AwaitStep
\*
\* Run with the buggy config (Fixed = FALSE) to obtain a counterexample to
\* MonitorErrorObservedBeforeBlockingWrites. Run with the fixed config (Fixed = TRUE) to verify
\* the invariant holds.

EXTENDS ReshardingMonitorFailure

====================================================================================================
