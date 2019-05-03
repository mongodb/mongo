/**
 * Tests that flow control outputs a log when it is maximally engaged on some cadence.
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    const replSet = new ReplSetTest({name: "flow_control_logging", nodes: 3});
    replSet.startSet({
        setParameter: {
            enableFlowControl: true,
            flowControlSamplePeriod:
                1,  // Increase resolution to detect lag in a light write workload.
            flowControlWarnThresholdSeconds: 1,
            // Configure flow control to engage after one second of lag.
            flowControlTargetLagSeconds: 1,
            flowControlThresholdLagPercentage: 1,
            // Use a speedy no-op writer to avoid needing a robust background writer.
            writePeriodicNoops: true,
            periodicNoopIntervalSecs:
                2  // replSet.initiate() can hang with a one second interval for reasons.
        }
    });
    replSet.initiate();

    // Stop replication which will pin the commit point.
    for (let sec of replSet.getSecondaries()) {
        assert.commandWorked(sec.adminCommand({
            configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries",
            mode: "alwaysOn"
        }));
    }

    const timeoutMilliseconds = 30 * 1000;
    // The test has stopped replication and the primary's no-op writer is configured to create an
    // oplog entry every other second. Once the primary notices the sustainer rate is not moving, it
    // should start logging a warning once per second. This check waits for two log messages to make
    // sure the appropriate state variables are being reset.
    checkLog.containsWithCount(replSet.getPrimary(),
                               "Flow control is engaged and the sustainer point is not moving.",
                               2,
                               timeoutMilliseconds);

    // Restart replication so the replica set will shut down.
    for (let sec of replSet.getSecondaries()) {
        assert.commandWorked(sec.adminCommand(
            {configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries", mode: "off"}));
    }

    replSet.stopSet();
})();
