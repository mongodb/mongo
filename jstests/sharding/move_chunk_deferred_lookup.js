/**
 * Ensure that updates are not lost if they are made between processing deferred updates and reading
 * from the updates list in _transferMods.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, requires_persistence]
 */

import {
    migrateStepNames,
    moveChunkParallel,
    moveChunkStepNames,
    pauseMigrateAtStep,
    unpauseMigrateAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const dbName = "test";
const collName = "user";
const staticMongod = MongoRunner.runMongod({});
const st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 1}}});
const collection = st.s.getDB(dbName).getCollection(collName);
const lsid = {
    id: UUID()
};
const txnNumber = 0;

function setup() {
    CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {_id: 1}, [
        {min: {_id: MinKey}, max: {_id: 10}, shard: st.shard0.shardName},
        {min: {_id: 10}, max: {_id: MaxKey}, shard: st.shard1.shardName},
    ]);

    for (let i = 0; i < 20; i++) {
        assert.commandWorked(collection.insertOne({_id: i, x: i}));
    }
}

function prepareTransactionAndTriggerFailover() {
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        update: collName,
        updates: [
            {q: {_id: 1}, u: {$set: {x: 5}}},
            {q: {_id: 2}, u: {$set: {x: -10}}},
        ],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));

    const result = assert.commandWorked(st.shard0.getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    }));

    let oldSecondary = st.rs0.getSecondary();

    st.rs0.stepUp(oldSecondary);

    awaitRSClientHosts(st.s, oldSecondary, {ok: true, ismaster: true});

    return result.prepareTimestamp;
}

function commitPreparedTransaction(prepareTimestamp) {
    assert.commandWorked(
        st.shard0.getDB(dbName).adminCommand(Object.assign({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        },
                                                           {commitTimestamp: prepareTimestamp})));
}

function runMoveChunkAndCommitTransaction() {
    const joinMoveChunk = moveChunkParallel(
        staticMongod, st.s.host, {_id: 1}, null, 'test.user', st.shard1.shardName);
    pauseMigrateAtStep(st.shard1, migrateStepNames.catchup);
    waitForMoveChunkStep(st.shard0, moveChunkStepNames.startedMoveChunk);
    commitPreparedTransaction(prepareTimestamp);
    unpauseMigrateAtStep(st.shard1, migrateStepNames.catchup);
    return joinMoveChunk;
}

setup();
const prepareTimestamp = prepareTransactionAndTriggerFailover();
const processingDeferredXferModsFp =
    configureFailPoint(st.rs0.getPrimary(), "hangAfterProcessingDeferredXferMods");
const pauseBeforeCriticalSectionFp =
    configureFailPoint(st.rs0.getPrimary(), "hangBeforeEnteringCriticalSection", {}, "alwaysOn");
const joinMoveChunk = runMoveChunkAndCommitTransaction();
processingDeferredXferModsFp.wait();
assert.commandWorked(collection.update({_id: 4}, {$set: {x: 501}}));
processingDeferredXferModsFp.off();
pauseBeforeCriticalSectionFp.off();
joinMoveChunk();
assert.eq(collection.findOne({_id: 4}).x, 501);

st.stop();

MongoRunner.stopMongod(staticMongod);