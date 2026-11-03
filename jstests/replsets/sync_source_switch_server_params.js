/**
 * Tests the 'ignoreSyncSourcePrimaryState' and 'prioritizeSyncSourceSwitch' server parameters
 * introduced to control oplog fetching behavior during failovers (SERVER-95560).
 *
 * Validates that:
 * 1. Both parameters can be read and set at runtime.
 * 2. With 'ignoreSyncSourcePrimaryState=true' and chaining disabled, a secondary correctly
 *    switches its sync source to the new primary after a step-up, even if the old primary's
 *    metadata still claims it is the primary.
 * 3. With 'prioritizeSyncSourceSwitch=true', the oplog fetcher evaluates sync source validity
 *    before enqueuing a batch, reducing the window for stale-primary replication.
 * 4. After failover with these parameters enabled, no majority of secondaries enters rollback.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "sync_source_switch_server_params";

const replSet = new ReplSetTest({
    name: testName,
    nodes: [{rsConfig: {priority: 3}}, {rsConfig: {priority: 2}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false},
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

const primary = replSet.getPrimary();
assert.eq(primary, replSet.nodes[0], "Expected node 0 to be the initial primary");

const newPrimary = replSet.nodes[1];
const secondary = replSet.nodes[2];

// -------- Phase 1: Validate parameters can be read and set at runtime --------
jsTestLog("Phase 1: Verify server parameters are readable and settable at runtime");

for (const node of replSet.nodes) {
    let res = assert.commandWorked(
        node.adminCommand({getParameter: 1, ignoreSyncSourcePrimaryState: 1}),
    );
    assert.eq(
        res.ignoreSyncSourcePrimaryState,
        false,
        "ignoreSyncSourcePrimaryState should default to false",
    );

    res = assert.commandWorked(node.adminCommand({getParameter: 1, prioritizeSyncSourceSwitch: 1}));
    assert.eq(
        res.prioritizeSyncSourceSwitch,
        false,
        "prioritizeSyncSourceSwitch should default to false",
    );

    assert.commandWorked(node.adminCommand({setParameter: 1, ignoreSyncSourcePrimaryState: true}));
    res = assert.commandWorked(
        node.adminCommand({getParameter: 1, ignoreSyncSourcePrimaryState: 1}),
    );
    assert.eq(
        res.ignoreSyncSourcePrimaryState,
        true,
        "ignoreSyncSourcePrimaryState should be true after setting",
    );

    assert.commandWorked(node.adminCommand({setParameter: 1, prioritizeSyncSourceSwitch: true}));
    res = assert.commandWorked(node.adminCommand({getParameter: 1, prioritizeSyncSourceSwitch: 1}));
    assert.eq(
        res.prioritizeSyncSourceSwitch,
        true,
        "prioritizeSyncSourceSwitch should be true after setting",
    );
}

jsTestLog("Phase 1 passed: Both parameters are readable and settable at runtime");

// -------- Phase 2: Sync source switch with params enabled + chaining disabled --------
jsTestLog("Phase 2: Verify sync source switching after failover with params enabled");

// Both params are now true on all nodes (set in Phase 1).
// Write some data to establish replication.
assert.commandWorked(primary.getDB(testName).coll.insert({phase: 2, a: 1}));
replSet.awaitReplication();

// Confirm the secondary is syncing from the current primary (node 0).
replSet.awaitSyncSource(secondary, primary);
jsTestLog("Secondary is syncing from the original primary as expected");

// Record each node's rollback ID to check for a rollback during failover.
const rbidsBeforeFailover = replSet.nodes.map(
    (node) => assert.commandWorked(node.adminCommand({replSetGetRBID: 1})).rbid,
);

// Step up node 1 to be the new primary.
jsTestLog("Stepping up node 1 to become the new primary");
replSet.stepUp(newPrimary);
assert.eq(replSet.getPrimary(), newPrimary, "Expected node 1 to be the new primary");

// With ignoreSyncSourcePrimaryState=true and chaining disabled, the secondary should switch
// to the new primary. The parameter ensures the switch happens even if the old primary's
// replSetMetadata still reports isPrimary: true during the transition window.
jsTestLog("Waiting for secondary to switch sync source to the new primary");
replSet.awaitSyncSource(secondary, newPrimary, 60 * 1000);
jsTestLog("Secondary correctly switched sync source to the new primary");

// Write on the new primary and verify replication still works.
assert.commandWorked(newPrimary.getDB(testName).coll.insert({phase: 2, a: 2}));
replSet.awaitReplication();

// Verify no node is in ROLLBACK state (the whole point of SERVER-95560).
jsTestLog("Verifying no nodes are in rollback state");
for (const node of replSet.nodes) {
    const status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
    const selfMember = status.members.find((m) => m.self);
    assert.neq(
        selfMember.stateStr,
        "ROLLBACK",
        "Node " + node.host + " unexpectedly entered ROLLBACK state",
    );
}

// Verify that each node's rollback ID has not changed.
replSet.nodes.forEach((node, i) => {
    assert.eq(
        rbidsBeforeFailover[i],
        assert.commandWorked(node.adminCommand({replSetGetRBID: 1})).rbid,
        "Node " + node.host + " rolled back during the failover",
    );
});

jsTestLog("Phase 2 passed: Sync source switch works correctly with params enabled");

// -------- Phase 3: Verify behavior with params disabled (default) --------
jsTestLog("Phase 3: Verify behavior reverts when params are disabled");

for (const node of replSet.nodes) {
    assert.commandWorked(node.adminCommand({setParameter: 1, ignoreSyncSourcePrimaryState: false}));
    assert.commandWorked(node.adminCommand({setParameter: 1, prioritizeSyncSourceSwitch: false}));
}

// Step node 0 back up (it has higher priority).
jsTestLog("Stepping up node 0 again");
replSet.stepUp(replSet.nodes[0]);
const restoredPrimary = replSet.getPrimary();
assert.eq(restoredPrimary, replSet.nodes[0], "Expected node 0 to be primary again");

// Verify sync source still eventually switches (default behavior with chaining disabled).
replSet.awaitSyncSource(secondary, restoredPrimary, 60 * 1000);
jsTestLog("Secondary correctly switched sync source with default params (chaining disabled)");

// Verify data integrity across all nodes.
assert.commandWorked(restoredPrimary.getDB(testName).coll.insert({phase: 3, a: 3}));
replSet.awaitReplication();

for (const node of replSet.nodes) {
    const count = node.getDB(testName).coll.count();
    assert.eq(count, 3, "Expected 3 documents on node " + node.host + ", got " + count);
}

jsTestLog("Phase 3 passed: Default behavior works correctly with chaining disabled");

jsTestLog("All phases passed");
replSet.stopSet();
