// Regression test to ensure that we don't crash during a getMore if the client's
// lastKnownCommittedOpTime switches from being ahead of the node's lastCommittedOpTime to behind
// while a tailable awaitData query is running. See SERVER-35239. This also tests that when the
// client's lastKnownCommittedOpTime is behind the node's lastCommittedOpTime, getMore returns early
// with an empty batch.
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

const name = "awaitdata_getmore_new_last_committed_optime";
const replSet = new ReplSetTest({name: name, nodes: 5, settings: {chainingAllowed: false}});

replSet.startSet();
replSet.initiate();

const dbName = "test";
const collName = "coll";

const primary = replSet.getPrimary();
const secondaries = replSet.getSecondaries();
const secondary = secondaries[0];

const primaryDB = primary.getDB(dbName);
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// Create capped collection on primary and allow it to be committed.
assert.commandWorked(primaryDB.createCollection(collName, {capped: true, size: 2048}));
replSet.awaitReplication();
replSet.awaitLastOpCommitted();

// Stop data replication on 3 secondaries to prevent writes being committed.
jsTestLog("Stopping replication");
stopServerReplication(secondaries[1]);
stopServerReplication(secondaries[2]);
stopServerReplication(secondaries[3]);

// Write data to primary.
for (let i = 0; i < 2; i++) {
    assert.commandWorked(primaryDB[collName].insert({_id: i}, {writeConcern: {w: 2}}));
}

replSet.awaitReplication(null, null, [secondary]);
jsTestLog("Secondary has replicated data");

jsTestLog("Starting parallel shell");
// Start a parallel shell because we'll be enabling a failpoint that will make the thread hang.
let waitForGetMoreToFinish = startParallelShell(async () => {
    const {getLastOpTime} = await import("jstests/replsets/rslib.js");
    const {ReplSetTest} = await import("jstests/libs/replsettest.js");

    const secondary = db.getMongo();
    secondary.setSecondaryOk();

    const dbName = "test";
    const collName = "coll";
    const awaitDataDB = db.getSiblingDB("test");

    // Create awaitData cursor and get all data written so that a following getMore will have to
    // wait for more data.
    let cmdRes = awaitDataDB.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, dbName + "." + collName);
    assert.eq(cmdRes.cursor.firstBatch.length, 2, tojson(cmdRes));

    // Enable failpoint.
    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "planExecutorHangBeforeShouldWaitForInserts",
            mode: "alwaysOn",
            data: {namespace: dbName + "." + collName},
        }),
    );

    // Call getMore on awaitData cursor with lastKnownCommittedOpTime ahead of node. This will
    // hang until we've disabled the failpoint. Set awaitData timeout "maxTimeMS" to use a large
    // timeout so we can test if the getMore command returns early on a stale
    // lastKnownCommittedOpTime.
    const lastOpTime = getLastOpTime(secondary);
    const cursorId = cmdRes.cursor.id;
    cmdRes = awaitDataDB.runCommand({
        getMore: cursorId,
        collection: collName,
        batchSize: NumberInt(2),
        maxTimeMS: ReplSetTest.kDefaultTimeoutMS,
        lastKnownCommittedOpTime: lastOpTime,
    });

    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, cursorId);
    assert.eq(cmdRes.cursor.ns, dbName + "." + collName);
    // Test that getMore returns early with an empty batch even though there was a new document
    // inserted.
    assert.eq(cmdRes.cursor.nextBatch.length, 0, tojson(cmdRes));
}, secondary.port);

// Ensure that we've hit the failpoint before moving on.
checkLog.contains(secondary, "PlanExecutor - planExecutorHangBeforeShouldWaitForInserts fail point enabled");

// Restart replication on the other nodes.
jsTestLog("Restarting replication");
restartServerReplication(secondaries[1]);
restartServerReplication(secondaries[2]);
restartServerReplication(secondaries[3]);

// Do another write to advance the optime so that the test client's lastKnownCommittedOpTime is
// behind the node's lastCommittedOpTime once all nodes catch up.
jsTestLog("Do another write after restarting replication");
assert.commandWorked(primaryDB[collName].insert({_id: 2}));

// Wait until all nodes have committed the last op. At this point in executing the getMore,
// the node's lastCommittedOpTime should now be ahead of the client's lastKnownCommittedOpTime.
replSet.awaitLastOpCommitted();
jsTestLog("All nodes caught up");

// Disable failpoint.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "planExecutorHangBeforeShouldWaitForInserts", mode: "off"}),
);

waitForGetMoreToFinish();
jsTestLog("Parallel shell successfully exited");

replSet.stopSet();
