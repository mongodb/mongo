/**
 * Tests that the oplog history for a retryable findAndModify can be fetched and migrated correctly
 * during resharding critical section.
 *
 * 1. Pause resharding before entering the critical section.
 * 2. Pause the oplog fetcher on the recipient.
 * 3. Execute a findAndModify as a retryable write.
 * 4. Unpause resharding. Wait for it to enter the critical section.
 * 5. Resume the oplog fetcher.
 * 6. Wait for resharding to complete.
 * 7. Retry the findAndModify and verify the response is identical and the write was not
 *   re-executed.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);
const testColl = testDB.getCollection(collName);

// Make shard0 the primary shard for the test database.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create the test collection and insert a document.
assert.commandWorked(testColl.insert({_id: 1, x: 1, counter: 0}));

const configPrimary = st.configRS.getPrimary();
const donorPrimary = st.rs0.getPrimary();
const recipientPrimary = st.rs1.getPrimary();

const pauseBeforeStartingCriticalSectionFp = configureFailPoint(
    configPrimary,
    "reshardingPauseCoordinatorBeforeBlockingWrites",
);

jsTest.log("Starting moveCollection");
const moveCollThread = new Thread(
    (mongosHost, ns, toShard) => {
        const mongos = new Mongo(mongosHost);
        return mongos.adminCommand({moveCollection: ns, toShard: toShard});
    },
    st.s.host,
    ns,
    st.shard1.shardName,
);
moveCollThread.start();

jsTest.log("Waiting for resharding to pause before blocking writes");
pauseBeforeStartingCriticalSectionFp.wait();

jsTest.log("Waiting for the oplog fetcher on the recipient to pause after fetching some oplog entries");
const pauseOplogFetcherFp = configureFailPoint(recipientPrimary, "pauseReshardingOplogFetcherAfterConsuming");
// Perform an insert so there is an oplog entry for the recipient to fetch.
assert.commandWorked(testDB.runCommand({insert: collName, documents: [{_id: 2, x: 2, counter: 0}]}));
pauseOplogFetcherFp.wait();

jsTest.log("Performing findAndModify as a retryable write");
const lsid = {id: UUID()};
const txnNumber = NumberLong(1);

const findAndModifyCmd = {
    findAndModify: collName,
    query: {_id: 1},
    update: {$inc: {counter: 1}},
    new: true,
    lsid: lsid,
    txnNumber: txnNumber,
};

const initialRes = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
jsTest.log("Initial findAndModify response: " + tojson(initialRes));
assert.eq(initialRes.lastErrorObject.n, 1, initialRes);
assert.eq(initialRes.lastErrorObject.updatedExisting, true, initialRes);
assert.eq(initialRes.value.counter, 1, initialRes);
assert.eq(initialRes.value._id, 1, initialRes);

jsTest.log("Unpause resharding and wait for the critical section to start");
pauseBeforeStartingCriticalSectionFp.off();
assert.soon(() => {
    const docs = donorPrimary.getCollection("config.collection_critical_sections").find({_id: ns}).toArray();
    return docs.length > 0;
}, "Critical section did not start");

jsTest.log("Unpause the oplog fetcher on the recipient");
pauseOplogFetcherFp.off();

jsTest.log("Waiting for moveCollection to complete");
moveCollThread.join();
assert.commandWorked(moveCollThread.returnData());

// Retry the findAndModify. Verify the response is identical and the write was not re-executed.
const retriedRes = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
jsTest.log("Retried findAndModify response: " + tojson(retriedRes));
assert.docEq(retriedRes.lastErrorObject, initialRes.lastErrorObject, {retriedRes, initialRes});
assert.docEq(retriedRes.value, initialRes.value, {retriedRes, initialRes});

const doc = testColl.findOne({_id: 1});
assert.eq(doc.counter, 1, {doc});

st.stop();
