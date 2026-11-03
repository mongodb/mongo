/**
 * Simulates the SERVER-95560 scenario: during a failover the old primary's step-down is
 * blocked (via the 'blockHeartbeatStepdown' failpoint), so it continues serving oplog
 * batches whose metadata says isPrimary=true.
 *
 * Uses 'hangBeforeProcessingSuccessfulBatch' to freeze the secondary's oplog fetcher so
 * that an in-flight batch from the old primary survives the election.  After the election,
 * heartbeats inform the secondary about the new primary, but the paused oplog fetcher still
 * holds the old batch.
 *
 * With 'ignoreSyncSourcePrimaryState' disabled (default), shouldChangeSyncSource skips the
 * new-primary check when the sync source claims isPrimary=true, so the secondary stays stuck
 * on the stale primary -- this is the bug.
 *
 * Each scenario below starts with both parameters 'ignoreSyncSourcePrimaryState' and
 * 'prioritizeSyncSourceSwitch' disabled, holds the secondary's fetcher from before the election
 * until it has fetched a batch containing a write that only the old primary has, and then
 * enables some subset of the parameters at runtime on the secondary before releasing it:
 *
 * - Both parameters: the secondary switches to the real primary. With
 *   'prioritizeSyncSourceSwitch' enabled, the batch that triggers the switch is discarded
 *   before it is enqueued.
 * - Only 'ignoreSyncSourcePrimaryState': the secondary still switches, but the batch that
 *   triggers the switch is enqueued first.
 * - Only 'prioritizeSyncSourceSwitch': secondary doesn't switch.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

// How the secondary is expected to behave once the parameters are enabled.
const kSwitchToNewPrimary = "switchToNewPrimary";
const kStayOnPrimary = "stayOnPrimary";

function getSyncSourceHost(node) {
    const status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
    return status.members.find((m) => m.self).syncSourceHost;
}

function runScenario({
    name,
    ignoreSyncSourcePrimaryState,
    prioritizeSyncSourceSwitch,
    expectation,
    staleBatchDiscarded = false,
}) {
    const testName = "sync_source_switch_on_failover_" + name;
    jsTest.log.info("Starting scenario", {name, expectation});

    const replSet = new ReplSetTest({
        name: testName,
        nodes: [{rsConfig: {priority: 3}}, {rsConfig: {priority: 2}}, {rsConfig: {priority: 0}}],
        // Skip primary catch-up. The old primary cannot step down (blockHeartbeatStepdown) and keeps
        // writing noops in the old term, which it rejects oplog queries from the new term for, so
        // the new primary could never reach its catch-up target.
        settings: {chainingAllowed: false, catchUpTimeoutMillis: 0},
        nodeOptions: {
            setParameter: {
                writePeriodicNoops: true,
                periodicNoopIntervalSecs: 1,
                ignoreSyncSourcePrimaryState: false,
                prioritizeSyncSourceSwitch: false,
            },
        },
    });
    replSet.startSet();
    replSet.initiate();

    const nodes = replSet.nodes;
    const oldPrimary = replSet.getPrimary();
    assert.eq(oldPrimary, nodes[0], "Expected node 0 to be the initial primary");

    const oldPrimaryEndpoint = TestData.usePriorityPorts
        ? oldPrimary.priorityHost
        : oldPrimary.host;
    const newPrimary = nodes[1];
    const secondary = nodes[2];

    assert.commandWorked(oldPrimary.getDB(testName).coll.insert({phase: "baseline", i: 0}));
    replSet.awaitReplication();

    replSet.awaitSyncSource(secondary, oldPrimary);
    jsTest.log.info("Secondary is syncing from node 0 (old primary)");

    // ======================================================================
    // Step 1: Freeze the oplog fetcher, trigger election, verify the secondary is stuck.
    //
    // hangBeforeProcessingSuccessfulBatch pauses the oplog fetcher AFTER it
    // receives a batch from node 0 but BEFORE processing it.  The election
    // happens on vote/heartbeat threads while the fetcher is frozen, so the
    // cursor to node 0 stays alive.
    // ======================================================================
    jsTest.log.info("Activating hangBeforeProcessingSuccessfulBatch on the secondary");
    const hangFetcher = configureFailPoint(secondary, "hangBeforeProcessingSuccessfulBatch");

    jsTest.log.info("Activating blockHeartbeatStepdown on the old primary");
    const blockStepdown = configureFailPoint(oldPrimary, "blockHeartbeatStepdown");

    hangFetcher.wait();
    jsTest.log.info("Secondary oplog fetcher is paused with a batch from node 0");

    jsTest.log.info("Stepping up node 1 as new primary");
    assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
    assert.soon(
        function () {
            return newPrimary.adminCommand("hello").isWritablePrimary;
        },
        "node 1 should become writable primary",
        60 * 1000,
    );

    // Wait for the secondary to learn that node 1 is primary.
    jsTest.log.info("Waiting for the secondary to learn about the new primary");
    assert.soon(function () {
        const status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        return status.members.some((m) => m._id === 1 && m.stateStr === "PRIMARY");
    }, "secondary should see node 1 as primary");

    // Write conflicting data during the split-brain.  The old primary still
    // accepts writes because blockHeartbeatStepdown prevents state transition.
    // These writes will need to be rolled back once the old primary steps down
    // and syncs from the real primary.
    jsTest.log.info("Writing stale data to old primary during split-brain");
    assert.commandWorked(
        oldPrimary
            .getDB(testName)
            .stale.insert(
                {phase: "stale_split_brain", source: "old_primary"},
                {writeConcern: {w: 1}},
            ),
    );

    // Configure the fail point on fetcher to hold the batch containing the stale write.
    jsTest.log.info("Holding the oplog fetcher on the batch containing the stale data");
    const holdStaleBatchOnNextBatch = configureFailPoint(
        secondary,
        "hangBeforeProcessingSuccessfulBatch",
        {},
        {skip: 1},
    );
    holdStaleBatchOnNextBatch.wait();

    jsTest.log.info("Writing authoritative data to new primary during split-brain");
    assert.commandWorked(
        newPrimary
            .getDB(testName)
            .authoritative.insert(
                {phase: "new_primary_write", source: "new_primary"},
                {writeConcern: {w: 1}},
            ),
    );

    const rbidBefore = assert.commandWorked(oldPrimary.adminCommand({replSetGetRBID: 1})).rbid;
    const secondaryRbidBefore = assert.commandWorked(
        secondary.adminCommand({replSetGetRBID: 1}),
    ).rbid;
    jsTest.log.info("Old primary rollback ID before step-down", {rbidBefore});

    // ======================================================================
    // Step 2: Enable the requested parameters. The next shouldChangeSyncSource call
    // decides whether to leave the stale primary.
    // ======================================================================
    jsTest.log.info("Enabling parameters", {
        prioritizeSyncSourceSwitch,
        ignoreSyncSourcePrimaryState,
    });
    if (prioritizeSyncSourceSwitch) {
        assert.commandWorked(
            secondary.adminCommand({setParameter: 1, prioritizeSyncSourceSwitch: true}),
        );
    }
    if (ignoreSyncSourcePrimaryState) {
        assert.commandWorked(
            secondary.adminCommand({setParameter: 1, ignoreSyncSourcePrimaryState: true}),
        );
    }

    // The fetcher now processes the batch containing the stale write.
    holdStaleBatchOnNextBatch.off();

    if (expectation === kStayOnPrimary) {
        // A write on the old primary only reaches the secondary if the secondary processes more
        // batches from the old primary. This shows the secondary did not switch sync sources.
        assert.commandWorked(
            oldPrimary
                .getDB(testName)
                .coll.insert({phase: "after_parameters_enabled", i: 1}, {writeConcern: {w: 1}}),
        );
        assert.soon(
            function () {
                return (
                    secondary.getDB(testName).coll.findOne({phase: "after_parameters_enabled"}) !==
                    null
                );
            },
            "Secondary should keep replicating from the old primary",
            60 * 1000,
        );

        assert.eq(
            getSyncSourceHost(secondary),
            oldPrimaryEndpoint,
            "Secondary should not have left the old primary",
        );
        jsTest.log.info("Secondary correctly stayed on the old primary");
    } else {
        jsTest.log.info("Waiting for secondary to switch to the REAL primary");
        replSet.awaitSyncSource(secondary, newPrimary, 60 * 1000);
        jsTest.log.info("Secondary switched to the real primary");

        // Verify the oplog fetcher rejected the old primary as a sync source.
        checkLog.contains(secondary, ") is no longer valid");

        // Verify data integrity: writes from the real primary should replicate.
        assert.commandWorked(newPrimary.getDB(testName).coll.insert({phase: "after_fix", i: 2}));
        // Ensure that the secondary replicates the new write after switching sync sources.
        replSet.awaitSecondaryNodes(60 * 1000, [secondary]);
        assert.soon(
            function () {
                return secondary.getDB(testName).coll.findOne({phase: "after_fix"}) !== null;
            },
            "Secondary should replicate writes from the real primary",
            60 * 1000,
        );

        if (staleBatchDiscarded) {
            assert.eq(
                null,
                secondary.getDB(testName).stale.findOne({phase: "stale_split_brain"}),
                "Secondary should not have the stale write",
            );
            assert.eq(
                secondaryRbidBefore,
                assert.commandWorked(secondary.adminCommand({replSetGetRBID: 1})).rbid,
                "Secondary should not have rolled back",
            );
        }
    }

    // ======================================================================
    // Cleanup
    // ======================================================================
    jsTest.log.info("Cleaning up: releasing blockHeartbeatStepdown");
    try {
        blockStepdown.off();
    } catch (e) {
        // Step-down may close the connection before the command returns.
    }
    replSet.awaitSecondaryNodes(60 * 1000, [oldPrimary]);
    reconnect(oldPrimary);
    replSet.awaitReplication();

    // Verify the old primary underwent rollback: its rbid should have incremented
    // and the stale split-brain write should be gone.
    jsTest.log.info("Verifying old primary rolled back stale split-brain writes");
    const rbidAfter = assert.commandWorked(oldPrimary.adminCommand({replSetGetRBID: 1})).rbid;
    jsTest.log.info("Old primary rollback ID after step-down", {rbidAfter});
    assert.gt(
        rbidAfter,
        rbidBefore,
        "Old primary's rollback ID should have incremented (was " +
            rbidBefore +
            ", now " +
            rbidAfter +
            ")",
    );

    assert.eq(
        null,
        oldPrimary.getDB(testName).stale.findOne({phase: "stale_split_brain"}),
        "Stale split-brain write should have been rolled back on the old primary",
    );

    assert.neq(
        null,
        oldPrimary.getDB(testName).authoritative.findOne({phase: "new_primary_write"}),
        "Authoritative write from real primary should be present on old primary after sync",
    );

    jsTest.log.info("Scenario passed", {name});
    replSet.stopSet();
}

runScenario({
    name: "both_params",
    ignoreSyncSourcePrimaryState: true,
    prioritizeSyncSourceSwitch: true,
    expectation: kSwitchToNewPrimary,
    staleBatchDiscarded: true,
});

runScenario({
    name: "ignore_primary_state_only",
    ignoreSyncSourcePrimaryState: true,
    prioritizeSyncSourceSwitch: false,
    expectation: kSwitchToNewPrimary,
});

runScenario({
    name: "prioritize_switch_only",
    ignoreSyncSourcePrimaryState: false,
    prioritizeSyncSourceSwitch: true,
    expectation: kStayOnPrimary,
});
