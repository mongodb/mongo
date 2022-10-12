/*
 * Test that tenant access blockers are installed and prevent writes during shard split
 *
 * @tags: [requires_fcv_62, serverless]
 */

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/serverless/libs/shard_split_test.js");

(function() {
"use strict";

jsTestLog("Starting runBlocking");

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new ShardSplitTest({
    recipientTagName: "recipientNode",
    recipientSetName: "recipient",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const maxTimeMS = 1 * 2000;  // 2 seconds
const tenantIds = ["tenant1", "tenant2"];

jsTestLog("Asserting no state document exist before command");
const operation = test.createSplitOperation(tenantIds);
assert.isnull(findSplitOperation(donorPrimary, operation.migrationId));

jsTestLog("Asserting we can write before the migration");
tenantIds.forEach(id => {
    const tenantDB = donorPrimary.getDB(`${id}_data`);
    const insertedObj = {name: `${id}_1`, payload: "testing_data"};
    assert.commandWorked(
        tenantDB.runCommand({insert: "testing_collection", documents: [insertedObj]}));
});

// configure failpoints
const adminDb = donorPrimary.getDB("admin");
const blockingFailPoint = configureFailPoint(adminDb, "pauseShardSplitAfterBlocking");

jsTestLog("Running commitShardSplit command");
const runThread = operation.commitAsync();

blockingFailPoint.wait();

jsTestLog("Asserting state document is in blocking state");
assertMigrationState(donorPrimary, operation.migrationId, "blocking");

jsTestLog("Asserting we cannot write in blocking state");
let writeThreads = [];
tenantIds.forEach((id, index) => {
    // Send write commands in an individual thread
    writeThreads[index] = new Thread((host, id) => {
        const donorPrimary = new Mongo(host);
        const tenantDB = donorPrimary.getDB(`${id}_data`);
        const insertedObj = {name: `${id}_2`, payload: "testing_data2"};
        const res = tenantDB.runCommand({insert: "testing_collection", documents: [insertedObj]});
        jsTestLog("Get response for write command: " + tojson(res));
    }, donorPrimary.host, id);
    writeThreads[index].start();
});
// Poll the numBlockedWrites of tenant migration access blocker from donor and expect it's
// increased by the blocked write command.
assert.soon(function() {
    return tenantIds.every(id => {
        const mtab = donorPrimary.getDB('admin')
                         .adminCommand({serverStatus: 1})
                         .tenantMigrationAccessBlocker[id]
                         .donor;
        return mtab.numBlockedWrites > 0;
    });
}, "no blocked writes found", 10 * 1000, 1 * 1000);

jsTestLog("Disabling failpoints and waiting for command to complete");
blockingFailPoint.off();

runThread.join();
const data = runThread.returnData();
assert(data["ok"] == 1);

writeThreads.forEach(thread => thread.join());

jsTestLog("Asserting state document exist after command");
assertMigrationState(donorPrimary, operation.migrationId, "committed");

operation.forget();

test.waitForGarbageCollection(operation.migrationId, tenantIds);
test.stop();
})();
