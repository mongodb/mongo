/**
 * Tests that additional participants can be added to an existing transaction.
 * @tags: [requires_fcv_80]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

const verifyMidpointTransactionMetrics = function(initialTxnMetrics, expectedParticipants) {
    const expectedMongosTargetedShards =
        initialTxnMetrics.totalContactedParticipants + expectedParticipants;
    let midpointTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    // More transactions can be running in the background. Check we observe the minimum.
    assert.gte(midpointTxnMetrics.totalContactedParticipants, expectedMongosTargetedShards);
};

const verifyFinalTransactionMetrics = function(
    initialTxnMetrics, expectedParticipants, numWriteShards) {
    const finalTxnMetrics = assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
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

    // Check the number of participants at commit time was incremented by at least the number of
    // participants we expect in the transaction.
    assert.gte(finalTxnMetrics.totalParticipantsAtCommit,
               initialTxnMetrics.totalParticipantsAtCommit + expectedParticipants.length);
};

const setUpTestCase = function(addedParticipantIsExistingParticipantBeforeAgg,
                               fooDocsToInsert,
                               barDocsToInsert,
                               txnNum,
                               sessionId) {
    // Insert foo docs outside of the transaction
    assert.commandWorked(st.s.getDB(dbName).foo.insert(fooDocsToInsert));

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
            lsid: sessionId,
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

    return initialTxnMetrics;
};

const setHangFps = function(shardsToSetFp) {
    // Set a failpoint on each of the shards we expect the participant shard to target that will
    // force them to hang while executing the agg request.
    const hangFps = [];
    shardsToSetFp.forEach(shard => {
        hangFps.push(configureFailPoint(
            shard, "hangAfterAcquiringCollectionCatalog", {collection: foreignColl}));
    });

    return hangFps;
};

const runAgg = function(
    addedParticipantIsExistingParticipantBeforeAgg, lookupPipeline, txnNum, sessionId) {
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

    return aggRequestThread;
};

const testAddingParticipants = function(expectedParticipants,
                                        addedParticipantIsExistingParticipantBeforeAgg,
                                        fooDocsToInsert,
                                        barDocsToInsert,
                                        lookupPipeline,
                                        shardsWithForeignColl) {
    const session = st.s.startSession();
    const txnNum = 1;
    const sessionId = session.getSessionId();

    const initialTxnMetrics = setUpTestCase(addedParticipantIsExistingParticipantBeforeAgg,
                                            fooDocsToInsert,
                                            barDocsToInsert,
                                            txnNum,
                                            sessionId);
    const hangFps = setHangFps(shardsWithForeignColl);
    const aggRequestThread =
        runAgg(addedParticipantIsExistingParticipantBeforeAgg, lookupPipeline, txnNum, sessionId);

    // In order to assert that mongos did not target the shards with the foreign collection itself,
    // wait to hit the failpoint on each shard with the foreign collection, then check that mongos
    // has only bumped its 'totalContactedParticipants' by 1 to account for the shard that owns
    // the "local" collection.
    hangFps.forEach(fp => {
        fp.wait();
        verifyMidpointTransactionMetrics(initialTxnMetrics, 1);
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
    verifyFinalTransactionMetrics(initialTxnMetrics, expectedParticipants, numWriteShards);

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

print(
    "Testing that a participant added on a getMore without a readOnly value participates in commit protocol");
// Move chunks such that the localColl has chunks:
// shard0: _id: [0, +inf]
// shard1: _id: [-inf, 0]
assert.commandWorked(st.s.adminCommand({split: localNs, middle: {_id: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: localNs, find: {_id: 10}, to: shard1.shardName}));

// Move chunks such that the foreignColl has chunks:
// shard1: x: [-inf, 0]
// shard2: x: [0, +inf]
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: shard1.shardName}));

[shard0, shard1, shard2].forEach(shard => {
    assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
});
st.refreshCatalogCacheForNs(st.s, foreignNs);

// Run an agg request such that:
// - Mongos will target shard0 and shard1 who own the localColl
// - Shard0 will target shard1 only. Shard0 will only target shard1 because shard0 owns docs with
//   positive "_id" values in the localColl, and shard1 owns docs with positive "x" values in the
//   foreign coll. Shard0 will turn the pipeline into a $match to run against shard1, and return
//   docs whose foreignColl "x" values matches the localColl "_id" values.
// - Shard1 will target shard2 only for the same reason, except that shard1 owns docs with negative
//   "_id" calues in the localColl, and shard2 owns docs with negative "x" values in the foreignColl

fooDocsToInsert = [];
barDocsToInsert = [];
for (let i = -200; i <= 200; i++) {
    fooDocsToInsert.push({_id: i});
    barDocsToInsert.push({x: i});
}
lookupPipeline = [
    {$lookup: {from: foreignColl, localField: "_id", foreignField: "x", as: "result"}},
    {$limit: NumberInt(200)}
];
let shardsToSetHangFp = [st.shard1, st.shard2];

const session = st.s.startSession();
let txnNum = 1;
const sessionId = session.getSessionId();

let initialTxnMetrics = setUpTestCase(addedParticipantIsExistingParticipantBeforeAgg,
                                      fooDocsToInsert,
                                      barDocsToInsert,
                                      txnNum,
                                      sessionId);

// We force only shard2 to hang when executing the request sent by shard1. The agg request will
// follow the following sequence of events:
// - Mongos will send agg and then getMore to shard0 and shard1 (the default initial batchSize is 0,
//   so mongos will always send a getMore).
// - Shard0 will contact shard1.
// - At the ~same time shard1 will contact shard2.
// - Shard1 will respond to shard0, and include shard2 in its additionalParticipants list. It will
//   not have a readOnly value for shard2 yet, because shard2 has not responded.
// - Shard1 will eventually return enough docs such that to satisfy the $limit, and shard0 will
//   respond to mongos with these results, and include both shard1 and shard2 in its additional
//   participants list. It will not have a readOnly value for shard2.
// - Mongos will mark shard2 as readOnly because it receives the response on a getMore request. It
//   will then return to client without shard1 ever having heard from shard2.

const hangFps = setHangFps(shardsToSetHangFp);
let aggRequestThread =
    runAgg(addedParticipantIsExistingParticipantBeforeAgg, lookupPipeline, txnNum, sessionId);

// Wait to hit the hang failpoint on shard1, and check that mongos has only contacted the two shards
hangFps[0].wait();
verifyMidpointTransactionMetrics(initialTxnMetrics,
                                 2 /* expectedNewParticipantsContactedByMongos */);

// Turn off the failpoint on shard1 so that shard1 will continue with the request. Wait for shard1
// to contact shard2 and for shard2 to hit the failpoint
hangFps[0].off();
hangFps[1].wait();

// While shard2 is still hanging, wait for the agg to finish, and then check the results.
aggRequestThread.join();
let aggRes = aggRequestThread.returnData();

// Assert that the response only includes docs from the foreign collection that live on shard1,
// meaning docs with x < 0.
aggRes.cursor.firstBatch.forEach((doc) => {
    assert.eq(doc.result.length, 1);
    assert.lt(doc.result[0].x, 0);
});

// Now check that mongos now tracks all 3 participants, before turning off the failpoint on shard2.
verifyMidpointTransactionMetrics(initialTxnMetrics,
                                 3 /* expectedNewParticipantsContactedByMongos */);
hangFps[1].off();

// Wait for the getMore to be cleaned up on shard1 to avoid a race between shard2 receiving
// commitTransaction and the cursor on shard1 being killed - if shard1 sends another getMore request
// to shard2 before shard1 receives commitTransaction, it's possible for shard2 to receive the
// request after it has received and processed the commit. If this happens, shard2 will respond with
// NoSuchTransaction to shard1 (since shard2 will have already committed), which will cause shard1
// to abort the transaction.
assert.soon(() => {
    return st.shard1.getDB("admin")
               .aggregate([
                   {$currentOp: {allUsers: true, idleCursors: true}},
                   {$match: {"command.getMore": {$exists: true}}},
                   {$match: {"ns": {$regex: dbName + "\."}}}
               ])
               .toArray()
               .length == 0;
}, `Timed out waiting for cursor to be cleaned up on shard1`);

// Commit the transaction and check the final metrics.
assert.commandWorked(st.s.getDB("admin").adminCommand({
    commitTransaction: 1,
    lsid: sessionId,
    txnNumber: NumberLong(txnNum),
    stmtId: NumberInt(0),
    autocommit: false,
}));
verifyFinalTransactionMetrics(initialTxnMetrics, [st.shard0, st.shard1, st.shard2], 0);

assert.commandWorked(st.s.getDB(dbName).foo.remove({}));
assert.commandWorked(st.s.getDB(dbName).bar.remove({}));

print("Testing a transaction in which an added participant throws a view resolution error");
// Move the localColl chunk away from  the primary shard so that the primary shard does not own any
// data from either the local or foreign colls.
//
// The localColl has chunks:
// shard1: _id: [0, +inf]
// shard1: _id: [-inf, 0]
//
// The foreignColl has chunks:
// shard1: x: [-inf, 0]
// shard2: x: [0, +inf]

assert.commandWorked(
    st.s.adminCommand({moveChunk: localNs, find: {_id: -10}, to: shard1.shardName}));

fooDocsToInsert = [{_id: -5}, {_id: 5}];              // will live on shard1
barDocsToInsert = [{_id: 1, x: -5}, {_id: 2, x: 5}];  // will live on shard1 and shard2
initialTxnMetrics = setUpTestCase(addedParticipantIsExistingParticipantBeforeAgg,
                                  fooDocsToInsert,
                                  barDocsToInsert,
                                  txnNum,
                                  sessionId);

// Create a simple view on the foreign collection
assert.commandWorked(st.s.getDB(dbName).createView("foreignView", foreignColl, []));

// Refresh shard0 to prevent it from getting a StaleConfig error before the expected view resolution
// error
assert.commandWorked(
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: dbName + ".foreignView"}));

// Run $lookup against the view. Force the merging shard to be a non-primary shard so that the
// primary shard will not be a participant in the transaction at all. The expected behavior for this
// agg request is:
// 1. Mongos forwards the agg to shard1
// 2. Shard1 sends the agg request to shard0 (primary shard) because it treats "foreignView" as an
//    unsharded collection.
// 3. Shard0 throws CommandOnShardedViewNotSupportedOnMongod which contains info on how to resolve
//    the view. This was the first request shard0 had received in this transaciton, so it aborts
//    the transaction on itself.
// 4. Shard1 clears shard0 from its participants list, and uses the info from shard0 to target
//    itself and shard2.
// 5. The transaction finishes with only shard1 and shard2 as participants.
lookupPipeline = [
    {$_internalSplitPipeline: {mergeType: {"specificShard": st.shard1.shardName}}},
    {$lookup: {
            from: "foreignView",
            let: {localVar: "$_id"},
            pipeline: [
                {$match : {"_id": {$gte: -5}}},
            ],
            as: "result"}}
];
txnNum = 2;

// Get the initial primary shard metrics before running the agg request
let initialPrimaryShardTxnMetrics =
    assert.commandWorked(st.shard0.getDB("admin").adminCommand({serverStatus: 1})).transactions;

aggRequestThread =
    runAgg(addedParticipantIsExistingParticipantBeforeAgg, lookupPipeline, txnNum, sessionId);
aggRequestThread.join();
aggRes = aggRequestThread.returnData();

let primaryShardTxnMetrics =
    assert.commandWorked(st.shard0.getDB("admin").adminCommand({serverStatus: 1})).transactions;
assert.eq(primaryShardTxnMetrics.currentOpen, 0);
assert.eq(initialPrimaryShardTxnMetrics.totalAborted + 1, primaryShardTxnMetrics.totalAborted);

assert.commandWorked(st.s.getDB("admin").adminCommand({
    commitTransaction: 1,
    lsid: sessionId,
    txnNumber: NumberLong(txnNum),
    stmtId: NumberInt(0),
    autocommit: false,
}));

verifyFinalTransactionMetrics(initialTxnMetrics, [st.shard1, st.shard2], 0);

// Assert that the transaction was started and then aborted on the primary shard
primaryShardTxnMetrics =
    assert.commandWorked(st.shard0.getDB("admin").adminCommand({serverStatus: 1})).transactions;
assert.eq(initialPrimaryShardTxnMetrics.totalStarted + 1, primaryShardTxnMetrics.totalStarted);
assert.eq(initialPrimaryShardTxnMetrics.totalAborted + 1, primaryShardTxnMetrics.totalAborted);

st.stop();
