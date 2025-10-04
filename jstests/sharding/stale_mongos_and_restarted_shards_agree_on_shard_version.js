/**
 * Tests that after a restart of a shard, multi write operations, finds and aggregations still work
 * as expected with a stale router. Requrires persistence because it asumes the shard will still
 * have it's data after a restart.
 *
 * @tags: [
 *   requires_persistence,
 * ]
 *
 */
import {withRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on the shards and cause them to refresh theirsharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false},
};

const st = new ShardingTest({shards: 2, mongos: 2, other: {configOptions: nodeOptions}});

// Used to get the shard destination ids for the moveChunks commands
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;

const kDatabaseName = "TestDB";
st.enableSharding(kDatabaseName, st.shard1.shardName);

// Creates and shard collName with 2 chunks, one per shard. Only the router referenced by st.s0
// knows that collName is sharded, and all the shards are restarted so they don't have the
// collection's sharding status
function setupCollectionForTest(collName) {
    const ns = kDatabaseName + "." + collName;
    assert.commandFailedWithCode(st.s0.adminCommand({getShardVersion: ns}), [
        ErrorCodes.NamespaceNotSharded,
        ErrorCodes.NamespaceNotFound,
    ]);
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}), [
        ErrorCodes.NamespaceNotSharded,
        ErrorCodes.NamespaceNotFound,
    ]);
    st.shardCollection(ns, {Key: 1});

    st.s0.adminCommand({split: ns, middle: {Key: 0}});
    st.s0.adminCommand({moveChunk: ns, find: {Key: -1}, to: shard0Name});
    st.s0.adminCommand({moveChunk: ns, find: {Key: 0}, to: shard1Name});
    assert.commandFailedWithCode(st.s1.adminCommand({getShardVersion: ns}), [
        ErrorCodes.NamespaceNotSharded,
        ErrorCodes.NamespaceNotFound,
    ]);

    // This document will go to shard 0
    assert.commandWorked(st.s0.getDB(kDatabaseName).getCollection(collName).insert({Key: -1, inc: 0}));
    // This document will go to shard 1
    assert.commandWorked(st.s0.getDB(kDatabaseName).getCollection(collName).insert({Key: 0, inc: 0}));

    st.restartShardRS(0);
    st.restartShardRS(1);
}

const freshMongoS = st.s0;
const staleMongoS = st.s1;

{
    jsTest.log("Testing: Insert with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestInsertColl");

    let insertBulkOp = staleMongoS.getDB(kDatabaseName).TestInsertColl.initializeUnorderedBulkOp();
    insertBulkOp.insert({Key: -2});
    insertBulkOp.insert({Key: 1});
    insertBulkOp.execute();

    assert.eq(4, freshMongoS.getDB(kDatabaseName).TestInsertColl.find().itcount());
    assert.eq(4, staleMongoS.getDB(kDatabaseName).TestInsertColl.find().itcount());
}
{
    jsTest.log("Testing: Multi-update with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestUpdateColl");

    assert.commandWorked(staleMongoS.getDB(kDatabaseName).TestUpdateColl.update({}, {$inc: {inc: 1}}, {multi: true}));

    let s0Doc = freshMongoS.getDB(kDatabaseName).TestUpdateColl.findOne({Key: -1});
    assert.eq(1, s0Doc.inc);
    let s1Doc = freshMongoS.getDB(kDatabaseName).TestUpdateColl.findOne({Key: 0});
    assert.eq(1, s1Doc.inc);
}
{
    jsTest.log("Testing: Multi-remove with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestRemoveColl");

    assert.commandWorked(staleMongoS.getDB(kDatabaseName).TestRemoveColl.remove({}, {justOne: false}));

    assert.eq(0, freshMongoS.getDB(kDatabaseName).TestRemoveColl.find().itcount());
}
{
    jsTest.log("Testing: Find-and-modify with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestFindAndModifyColl");

    assert.eq(
        null,
        staleMongoS.getDB(kDatabaseName).TestFindAndModifyColl.findAndModify({
            query: {Key: -2},
            update: {Key: -2},
            upsert: true,
        }),
    );

    assert.eq({Key: -2}, freshMongoS.getDB(kDatabaseName).TestFindAndModifyColl.findOne({Key: -2}, {_id: 0}));
}
{
    jsTest.log("Testing: Find with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestFindColl");

    let coll = staleMongoS.getDB(kDatabaseName).TestFindColl.find().toArray();
    assert.eq(2, coll.length);
}
{
    jsTest.log("Testing: Aggregate with sharded collection unknown on a stale mongos");
    setupCollectionForTest("TestAggregateColl");

    let count = staleMongoS
        .getDB(kDatabaseName)
        .TestAggregateColl.aggregate([{$count: "total"}])
        .toArray();
    assert.eq(2, count[0].total);
}
let session = null;
withRetryOnTransientTxnError(
    () => {
        jsTest.log("Testing: Transactions with unsharded collection, which is unknown on the shard");
        st.restartShardRS(0);
        st.restartShardRS(1);

        session = staleMongoS.startSession();
        session.startTransaction();
        session.getDatabase(kDatabaseName).TestTransactionColl.insertOne({Key: 1});
        session.commitTransaction();
    },
    () => {
        session.abortTransaction();
        session.getDatabase(kDatabaseName).TestTransactionColl.drop();
    },
);

withRetryOnTransientTxnError(
    () => {
        jsTest.log("Testing: Create collection as first op inside transaction works");
        st.restartShardRS(0);
        st.restartShardRS(1);

        session = staleMongoS.startSession();
        session.startTransaction();
        session.getDatabase(kDatabaseName).createCollection("TestTransactionCollCreation");
        session.getDatabase(kDatabaseName).TestTransactionCollCreation.insertOne({Key: 0});
        session.commitTransaction();
    },
    () => {
        session.abortTransaction();
        session.getDatabase(kDatabaseName).TestTransactionCollCreation.drop();
    },
);

{
    const kNumThreadsForConvoyTest = 20;

    jsTest.log("Testing: Several concurrent StaleShardVersion(s) result in a single refresh");

    setupCollectionForTest("TestConvoyColl");
    // Insert one document per thread, we skip Key: -1 becase it was inserted on the set up. We pick
    // shard0 which will have all the negative numbers
    let bulk = freshMongoS.getDB(kDatabaseName).TestConvoyColl.initializeUnorderedBulkOp();
    for (let i = 2; i <= kNumThreadsForConvoyTest; ++i) {
        bulk.insert({Key: -i});
    }
    assert.commandWorked(bulk.execute());

    // Restart the shard to have UNKNOWN shard version.
    st.restartShardRS(0);

    // Anticipate a refresh of the logical session cache to avoid the risk of it happening later by
    // affecting the actual number of refreshing threads and sharding statistics. In sharded
    // clusters, the logical session collection is sharded and any operations on it require the
    // cached metadata to be updated, causing a refresh if necessary.
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: "config.system.sessions"});

    let failPoint = configureFailPoint(st.shard0, "hangInRecoverRefreshThread");

    const parallelCommand = function (kDatabaseName, kCollectionName, i) {
        assert.commandWorked(
            db
                .getSiblingDB(kDatabaseName)
                .getCollection(kCollectionName)
                .updateOne(
                    {Key: i},
                    {
                        $set: {a: 1},
                    },
                ),
        );
    };

    let updateShells = [];

    for (let i = 1; i <= kNumThreadsForConvoyTest; ++i) {
        updateShells.push(
            startParallelShell(funWithArgs(parallelCommand, kDatabaseName, "TestConvoyColl", -i), staleMongoS.port),
        );
    }

    failPoint.wait();

    let matchingOps;
    assert.soon(() => {
        matchingOps = st.shard0
            .getDB("admin")
            .aggregate([
                {$currentOp: {"allUsers": true, "idleConnections": true}},
                {$match: {"command.update": "TestConvoyColl"}},
            ])
            .toArray();
        // Wait until all operations are blocked waiting for the refresh.
        return kNumThreadsForConvoyTest === matchingOps.length && matchingOps[0].opid != null;
    }, "Failed to find operations");

    let shardOps = st.shard0
        .getDB("admin")
        .aggregate([
            {$currentOp: {"allUsers": true, "idleConnections": true}},
            {$match: {desc: {$regex: "RecoverRefreshThread"}}},
        ])
        .toArray();

    // There must be only one thread refreshing.
    assert.eq(1, shardOps.length);

    failPoint.off();

    updateShells.forEach((updateShell) => {
        updateShell();
    });

    // All updates must succeed on all documents.
    assert.eq(kNumThreadsForConvoyTest, freshMongoS.getDB(kDatabaseName).TestConvoyColl.countDocuments({a: 1}));
}

st.stop();
