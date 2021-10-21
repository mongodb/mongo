/**
 * Tests that we are able to roll back immediately after replSetInitiate.
 *
 * @tags: [
 *  requires_persistence,
 *  multiversion_incompatible,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

const rst = ReplSetTest({
    name: jsTestName(),
    nodes: [
        {
            setParameter: {
                "failpoint.pauseCheckpointThread": tojson({mode: "alwaysOn"}),
                // We will not be able to rebuild primary only services as those ultimately
                // require the checkpointer to be running.
                "failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"}),
                "logComponentVerbosity": tojson({replication: 3}),
            },
            rsConfig: {priority: 2},
        },
        {
            rsConfig: {priority: 1},
        },
        {
            rsConfig: {priority: 0},
        },
    ],
    useBridge: true
});

rst.startSet();
const config = rst.getReplSetConfig();

// We have to initiate manually as RST adds nodes one-by-one, which can lead to the first
// node taking a stable checkpoint.
assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: config}));

jsTestLog("Done initiating");

const node0 = rst.nodes[0];
const node1 = rst.nodes[1];
const node2 = rst.nodes[2];

const dbName = "testdb";
const collName = "testcoll";

// Make the first node the primary.
rst.stepUp(node0, {awaitReplicationBeforeStepUp: false});

const oldPrimary = rst.getPrimary();
assert.eq(node0, oldPrimary);
rst.awaitSecondaryNodes();
rst.awaitNodesAgreeOnConfigVersion();
rst.awaitReplication();

const oldPrimaryDB = oldPrimary.getDB(dbName);
const oldPrimaryColl = oldPrimaryDB.getCollection(collName);

jsTestLog("Stopping replication");
stopServerReplication(node1);
stopServerReplication(node2);

jsTestLog("Writing to old primary");
assert.commandWorked(oldPrimaryColl.insert({"old1": 1}, {writeConcern: {w: 1}}));
assert.commandWorked(oldPrimaryColl.insert({"old2": 2}, {writeConcern: {w: 1}}));

jsTestLog("Disconnecting old primary");
node0.disconnect(node1);
node0.disconnect(node2);
assert.commandWorked(oldPrimary.adminCommand({replSetStepDown: 10 * 60, force: true}));
rst.waitForState(rst.nodes[0], ReplSetTest.State.SECONDARY);

jsTestLog("Electing new primary");

restartServerReplication(node1);
restartServerReplication(node2);

const newPrimary = rst.getPrimary();
const lastNode = (newPrimary.host === node1.host) ? node2 : node1;

jsTestLog("Writing to new primary " + newPrimary.host);
const newPrimaryDB = newPrimary.getDB(dbName);
const newPrimaryColl = newPrimaryDB.getCollection(collName);
assert.commandWorked(newPrimaryColl.insert({"new1": 1}, {writeConcern: {w: 1}}));
assert.commandWorked(newPrimaryColl.insert({"new2": 2}, {writeConcern: {w: 1}}));
rst.awaitReplication(undefined /* timeout */, undefined /* secondaryOpTimeType */, [lastNode]);
rst.awaitLastOpCommitted(undefined /* timeout */, [lastNode]);

jsTestLog("Reconnecting old primary");
const lastRBID = assert.commandWorked(node0.adminCommand("replSetGetRBID")).rbid;
node0.reconnect(node1);
node0.reconnect(node2);

rst.waitForState(rst.nodes[0], ReplSetTest.State.ROLLBACK);

// We take a stable checkpoint at the end of rollback so we need the checkpointer to be running.
jsTestLog("Reenabling checkpointer so rollback can complete");
assert.commandWorked(
    rst.nodes[0].adminCommand({configureFailPoint: 'pauseCheckpointThread', mode: 'off'}));

assert.soonNoExcept(function() {
    const rbid = assert.commandWorked(node0.adminCommand("replSetGetRBID")).rbid;
    return rbid > lastRBID;
}, "rbid did not update", ReplSetTest.kDefaultTimeoutMS);

rst.waitForState(rst.nodes[0], ReplSetTest.State.SECONDARY);

jsTestLog("Done with test");
rst.stopSet();
})();
