/**
 * Tests that additional participants can be added to an existing transaction.
 * @tags: [
 *   featureFlagAllowAdditionalParticipants,
 *   temp_disabled_embedded_router_metrics,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

const verifyFinalTransactionMetrics = function(
    initialTxnMetrics, finalTxnMetrics, expectedParticipants, numWriteShards) {
    // Check we executed the expected commit type. Currently this test only runs transactions where
    // either a single shard, readOnly, or single write shard commit type should be executed
    if (expectedParticipants.length == 1) {
        assert.eq(finalTxnMetrics.commitTypes.singleShard.successful,
                  initialTxnMetrics.commitTypes.singleShard.successful + 1);
    } else if (numWriteShards == 0) {
        assert.eq(finalTxnMetrics.commitTypes.readOnly.successful,
                  initialTxnMetrics.commitTypes.readOnly.successful + 1);
    } else if (numWriteShards == 1) {
        assert.eq(finalTxnMetrics.commitTypes.singleWriteShard.successful,
                  initialTxnMetrics.commitTypes.singleWriteShard.successful + 1);
    }

    // Check the number of participants at commit time was incremented by the number of participants
    // we expect in the transaction
    assert.eq(finalTxnMetrics.totalParticipantsAtCommit,
              initialTxnMetrics.totalParticipantsAtCommit + expectedParticipants.length);
};

const testAddingParticipants = function(expectedParticipants,
                                        addedParticipantIsExistingParticipantBeforeAgg,
                                        fooDocsToInsert,
                                        barDocsToInsert,
                                        lookupPipeline,
                                        shardsWithForeignColl) {
    // Insert foo docs outside of the transaction
    assert.commandWorked(st.s.getDB(dbName).foo.insert(fooDocsToInsert));

    const session = st.s.startSession();
    const txnNum = 1;
    const sessionId = session.getSessionId();

    // If the participant that the $lookup adds is supposed to be an existing participant in the
    // transaction already, insert the bar docs as part of the transaction. Otherwise, insert
    // them outside of the transaction.
    if (addedParticipantIsExistingParticipantBeforeAgg) {
        // Check that we actually have docs to insert into "bar", which indicates we will run
        // the $lookup using "bar" as the foreign collection. If we're running the $lookup
        // using "foo" as the local and foreign collection, it doesn't mean much if we add the shard
        // that owns "foo" a participant before we run the agg at all, so we assume this is a test
        // writer's error.
        assert.gt(barDocsToInsert.length, 0);

        assert.commandWorked(st.s.getDB(dbName).runCommand({
            insert: foreignColl,
            documents: barDocsToInsert,
            lsid: session.getSessionId(),
            txnNumber: NumberLong(txnNum),
            stmtId: NumberInt(0),
            autocommit: false,
            startTransaction: true
        }));
    } else {
        assert.commandWorked(st.s.getDB(dbName).bar.insert(barDocsToInsert));
    }

    const initialTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;

    // Set a failpoint on each of the shards we expect the participant shard to target that will
    // force them to hang while executing the agg request.
    const hangFps = [];
    shardsWithForeignColl.forEach(shard => {
        hangFps.push(configureFailPoint(
            shard, "hangAfterAcquiringCollectionCatalog", {collection: foreignColl}));
    });

    // Run the $lookup in another thread.
    const runAggRequest =
        (mongosConn, dbName, collName, pipeline, sessionId, txnNum, startTransaction) => {
            let mongos = new Mongo(mongosConn);
            const lsid = eval("(" + sessionId + ")");

            let aggCmd = {
                aggregate: collName,
                pipeline: pipeline,
                cursor: {},
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                stmtId: NumberInt(0),
                autocommit: false
            };
            if (startTransaction) {
                aggCmd = Object.merge(aggCmd, {startTransaction: true});
            }

            return mongos.getDB(dbName).runCommand(aggCmd);
        };
    let aggRequestThread = new Thread(runAggRequest,
                                      st.s.host,
                                      dbName,
                                      localColl,
                                      lookupPipeline,
                                      tojson(sessionId),
                                      txnNum,
                                      !addedParticipantIsExistingParticipantBeforeAgg);
    aggRequestThread.start();

    // In order to assert that mongos did not target the shards with the foreign collection itself,
    // wait to hit the failpoint on each shard with the foreign collection, then check that mongos
    // has only bumped its 'totalContactedParticipants' by 1 to account for the shard that owns
    // the "local" collection.
    const expectedMongosTargetedShards = initialTxnMetrics.totalContactedParticipants + 1;
    hangFps.forEach(fp => {
        fp.wait();

        let midpointTxnMetrics =
            assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
        assert.eq(midpointTxnMetrics.totalContactedParticipants, expectedMongosTargetedShards);

        fp.off();
    });

    // Check that the agg returns the expected results. We've set up the docs and $lookup that we
    // expect there to be a 1:1 mapping across the docs in the foreign coll and local coll in the
    // $lookup
    aggRequestThread.join();
    let aggRes = aggRequestThread.returnData();
    assert.eq(aggRes.cursor.firstBatch.length, barDocsToInsert.length);
    [...Array(barDocsToInsert.length).keys()].forEach((i) => {
        let next = aggRes.cursor.firstBatch[i];
        assert.eq(next.result.length, 1);
        assert.eq(next.result[0], barDocsToInsert[i]);
    });

    assert.commandWorked(st.s.getDB("admin").adminCommand({
        commitTransaction: 1,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(txnNum),
        stmtId: NumberInt(0),
        autocommit: false,
    }));

    let numWriteShards = addedParticipantIsExistingParticipantBeforeAgg ? 1 : 0;
    if (Array.contains(shardsWithForeignColl, st.shard0)) {
        // If shard0 contains the foreign coll, it means shard0 targeted itself (because shard0
        // always contains the local coll). When a shard targets itself, it will include itself as
        // an additional participant, but will not yet have a readOnly value so will get marked as
        // having an unknown readOnly value. This will ultimately cause the shard to get marked as a
        // write shard.
        numWriteShards++;
    }
    const finalTxnMetrics = assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    verifyFinalTransactionMetrics(
        initialTxnMetrics, finalTxnMetrics, expectedParticipants, numWriteShards);

    // Drop any docs inserted to prep for the next test case
    assert.commandWorked(st.s.getDB(dbName).foo.remove({}));
    assert.commandWorked(st.s.getDB(dbName).bar.remove({}));
};

let st = new ShardingTest({shards: 3});

const dbName = "test";
const localColl = "foo";
const foreignColl = "bar";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

let shard0 = st.shard0;
let shard1 = st.shard1;
let shard2 = st.shard2;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

// Create a sharded collection, "foo", with the following chunk:
// shard0: _id: [-inf, +inf)
assert.commandWorked(st.s.adminCommand({shardCollection: localNs, key: {_id: 1}}));

// Create a sharded collection, "bar", with the following chunks:
// shard1: x: [-inf, 0)
// shard2: x: [0, +inf)
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: shard2.shardName}));

// These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
// from the shards starting, aborting, and restarting the transaction due to needing to
// refresh after the transaction has started.
[shard0, shard1, shard2].forEach(shard => {
    assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: localNs}));
});
st.refreshCatalogCacheForNs(st.s, localNs);

[shard0, shard1, shard2].forEach(shard => {
    assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
});
st.refreshCatalogCacheForNs(st.s, foreignNs);

print("Testing that an existing participant can add one additional participant which was not " +
      "already an active participant");
let fooDocsToInsert = [{_id: -5}];        // will live on shard0
let barDocsToInsert = [{_id: 1, x: -5}];  // will live on shard1
let expectedParticipants = [0, 1];
let addedParticipantIsExistingParticipantBeforeAgg = false;
let lookupPipeline = [
    {$lookup: {from: foreignColl, localField: "_id", foreignField: "x", as: "result"}},
    {$sort: {"_id": 1}}
];
let shardsWithForeignColl = [st.shard1];
testAddingParticipants(expectedParticipants,
                       addedParticipantIsExistingParticipantBeforeAgg,
                       fooDocsToInsert,
                       barDocsToInsert,
                       lookupPipeline,
                       shardsWithForeignColl);

print("Testing that an existing participant can add one additional participant which was " +
      "already an active participant");
addedParticipantIsExistingParticipantBeforeAgg = true;
testAddingParticipants(expectedParticipants,
                       addedParticipantIsExistingParticipantBeforeAgg,
                       fooDocsToInsert,
                       barDocsToInsert,
                       lookupPipeline,
                       shardsWithForeignColl);

print("Testing that an existing participant can add multiple additional participants");
expectedParticipants = [0, 1, 2];
addedParticipantIsExistingParticipantBeforeAgg = false;
fooDocsToInsert = [{_id: -5}, {_id: 5}];              // will live on shard0
barDocsToInsert = [{_id: 1, x: -5}, {_id: 2, x: 5}];  // will live on shard1 and shard2
shardsWithForeignColl = [st.shard1, st.shard2];
testAddingParticipants(expectedParticipants,
                       addedParticipantIsExistingParticipantBeforeAgg,
                       fooDocsToInsert,
                       barDocsToInsert,
                       lookupPipeline,
                       shardsWithForeignColl);

print("Testing that an existing participant can add itself as an additional participant");
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: shard0.shardName}));
[shard0, shard1, shard2].forEach(shard => {
    assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
});
st.refreshCatalogCacheForNs(st.s, foreignNs);

fooDocsToInsert = [{_id: -5}];        // will live on shard0
barDocsToInsert = [{_id: 1, x: -5}];  // will live on shard0
expectedParticipants = [0, 2];
lookupPipeline = [
    {$lookup: {
        from: foreignColl,
        let: {localVar: "$_id"},
        pipeline: [
            {$match : {"_id": {$gte: -5}}},
        ], 
            as: "result"}},
    {$sort: {"_id": 1}}
];
shardsWithForeignColl = [st.shard0, st.shard2];
testAddingParticipants(expectedParticipants,
                       addedParticipantIsExistingParticipantBeforeAgg,
                       fooDocsToInsert,
                       barDocsToInsert,
                       lookupPipeline,
                       shardsWithForeignColl);

st.stop();
