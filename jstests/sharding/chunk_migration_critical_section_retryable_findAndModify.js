/**
 * Tests that the oplog history for a retryable findAndModify can be fetched and migrated correctly
 * during chunk migration critical section.
 *
 * 1. Pause migration before entering the critical section.
 * 2. Pause the session oplog fetching by pausing the _getNextSessionMods command on the donor.
 * 3. Execute a findAndModify as a retryable write.
 * 4. Unpause migration. Wait for it to enter the critical section.
 * 5. Resume the session oplog fetching.
 * 6. Wait for migration to complete.
 * 7. Retry the findAndModify and verify the response is identical and the write was not
 *   re-executed.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);
const testColl = testDB.getCollection(collName);

// Create a sharded collection with shard0 as the primary shard.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(testColl.insert({_id: 1, x: 1, counter: 0}));

const donorPrimary = st.rs0.getPrimary();
const isAuthoritativeShardsDDLEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB("admin"),
    "AuthoritativeShardsDDL",
);

const pauseSessionFetcherFp = configureFailPoint(
    donorPrimary,
    "pauseChunkMigrationSessionOplogFetching",
);
const hangBeforeCriticalSectionFp = configureFailPoint(
    donorPrimary,
    "hangBeforeEnteringCriticalSection",
);

jsTest.log("Starting moveChunk");
const moveChunkThread = new Thread(
    (mongosHost, ns, toShard) => {
        const mongos = new Mongo(mongosHost);
        return mongos.adminCommand({moveChunk: ns, find: {x: 1}, to: toShard});
    },
    st.s.host,
    ns,
    st.shard1.shardName,
);
moveChunkThread.start();

// Perform an insert so there is an oplog entry for the donor to fetch and send to the recipient.
assert.commandWorked(
    testDB.runCommand({insert: collName, documents: [{_id: 2, x: 1, counter: 0}]}),
);

jsTest.log("Waiting for the session oplog fetcher on the donor to pause");
pauseSessionFetcherFp.wait();

jsTest.log("Waiting for migration to pause before entering critical section");
hangBeforeCriticalSectionFp.wait();

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

const cacheColl = donorPrimary.getCollection("config.cache.collections");
const collDocBefore = cacheColl.findOne({_id: ns});
const criticalSectionCounterBefore = collDocBefore
    ? collDocBefore.enterCriticalSectionCounter || 0
    : 0;

function hasRecoverableCriticalSectionDoc() {
    return (
        donorPrimary.getCollection("config.collection_critical_sections").find({_id: ns}).toArray()
            .length > 0
    );
}

function legacyCriticalSectionCounterIncremented() {
    const collDocAfter = cacheColl.findOne({_id: ns});
    const criticalSectionCounterAfter = collDocAfter
        ? collDocAfter.enterCriticalSectionCounter || 0
        : 0;
    return criticalSectionCounterAfter > criticalSectionCounterBefore;
}

jsTest.log("Unpause migration and wait for the critical section to start");
hangBeforeCriticalSectionFp.off();
assert.soon(() => {
    if (isAuthoritativeShardsDDLEnabled) {
        return hasRecoverableCriticalSectionDoc();
    }
    return legacyCriticalSectionCounterIncremented() || hasRecoverableCriticalSectionDoc();
}, "Critical section did not start");

jsTest.log("Unpause the session oplog fetching");
pauseSessionFetcherFp.off();

jsTest.log("Waiting for moveChunk to complete");
moveChunkThread.join();
assert.commandWorked(moveChunkThread.returnData());

// Retry the findAndModify. Verify the response is identical and the write was not re-executed.
const retriedRes = assert.commandWorked(testDB.runCommand(findAndModifyCmd));
jsTest.log("Retried findAndModify response: " + tojson(retriedRes));
assert.docEq(retriedRes.lastErrorObject, initialRes.lastErrorObject, {retriedRes, initialRes});
assert.docEq(retriedRes.value, initialRes.value, {retriedRes, initialRes});

// Verify the document was only updated once.
const doc = testColl.findOne({_id: 1});
assert.eq(doc.counter, 1, {doc});

st.stop();
