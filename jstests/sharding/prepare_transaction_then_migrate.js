/**
 * Test that a migration will:
 * 1. Ignore multi-statement transaction prepare conflicts in the clone phase, and
 * 2. Pick up the changes for prepared transactions in the transfer mods phase.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, requires_persistence]
 */

(function() {
"use strict";
load('jstests/libs/chunk_manipulation_util.js');
load('jstests/replsets/rslib.js');
load('jstests/sharding/libs/create_sharded_collection_util.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "user";

const staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

const TestMode = {
    kBasic: 'basic',
    kWithStepUp: 'with stepUp',
    kWithRestart: 'with restart',
};

let runTest = function(testMode) {
    jsTest.log(`Running test in mode ${testMode}`);

    const st = new ShardingTest(
        {shards: {rs0: {nodes: testMode == TestMode.kWithStepUp ? 2 : 1}, rs1: {nodes: 1}}});
    const collection = st.s.getDB(dbName).getCollection(collName);

    CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
        {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
        {min: {x: 0}, max: {x: 1000}, shard: st.shard0.shardName},
        {min: {x: 1000}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);

    assert.commandWorked(collection.insert([
        {_id: 1, x: -1, note: "move into chunk range being migrated"},
        {_id: 2, x: -2, note: "keep out of chunk range being migrated"},
        {_id: 3, x: 50, note: "move out of chunk range being migrated"},
        {_id: 4, x: 100, note: "keep in chunk range being migrated"},
    ]));

    const lsid = {id: UUID()};
    const txnNumber = 0;
    let stmtId = 0;

    assert.commandWorked(st.s0.getDB(dbName).runCommand({
        insert: collName,
        documents: [
            {_id: 5, x: -1.01, note: "move into chunk range being migrated"},
            {_id: 6, x: -2.01, note: "keep out of chunk range being migrated"},
            {_id: 7, x: 50.01, note: "move out of chunk range being migrated"},
            {_id: 8, x: 100.01, note: "keep in chunk range being migrated"},
        ],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        startTransaction: true,
        autocommit: false,
    }));

    assert.commandWorked(st.s.getDB(dbName).runCommand({
        update: collName,
        updates: [
            {q: {x: -1}, u: {$set: {x: 5}}},
            {q: {x: -2}, u: {$set: {x: -10}}},
            {q: {x: 50}, u: {$set: {x: -20}}},
            {q: {x: 100}, u: {$set: {x: 500}}},
            {q: {x: -1.01}, u: {$set: {x: 5.01}}},
            {q: {x: -2.01}, u: {$set: {x: -10.01}}},
            {q: {x: 50.01}, u: {$set: {x: -20.01}}},
            {q: {x: 100.01}, u: {$set: {x: 500.01}}},
        ],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId++),
        autocommit: false,
    }));

    const res = assert.commandWorked(st.shard0.getDB(dbName).adminCommand({
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    }));

    let prepareTimestamp = res.prepareTimestamp;

    if (testMode == TestMode.kWithStepUp) {
        st.rs0.stepUp(st.rs0.getSecondary());

        // Wait for the config server to see the new primary.
        // TODO SERVER-74177 Remove this once retry on NotWritablePrimary is implemented.
        st.configRS.nodes.forEach((conn) => {
            awaitRSClientHosts(conn, st.rs0.getPrimary(), {ok: true, ismaster: true});
        });
    } else if (testMode == TestMode.kWithRestart) {
        TestData.skipCollectionAndIndexValidation = true;
        st.rs0.restart(st.rs0.getPrimary());
        st.rs0.waitForMaster();
        TestData.skipCollectionAndIndexValidation = false;

        assert.soon(() => {
            try {
                st.shard0.getDB(dbName).getCollection("dummy").findOne();
                return true;
            } catch (ex) {
                print("Caught expected once exception due to restart: " + tojson(ex));
                return false;
            }
        });
    }

    const joinMoveChunk =
        moveChunkParallel(staticMongod, st.s.host, {x: 1}, null, 'test.user', st.shard1.shardName);

    pauseMigrateAtStep(st.shard1, migrateStepNames.catchup);

    // The donor shard only ignores prepare conflicts while scanning over the shard key index. We
    // wait for donor shard to have finished buffering the RecordIds into memory from scanning over
    // the shard key index before committing the transaction. Notably, the donor shard doesn't
    // ignore prepare conflicts when fetching the full contents of the documents during calls to
    // _migrateClone.
    //
    // TODO: SERVER-71028 Remove comment after making changes.

    waitForMoveChunkStep(st.shard0, moveChunkStepNames.startedMoveChunk);

    assert.commandWorked(
        st.shard0.getDB(dbName).adminCommand(Object.assign({
            commitTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        },
                                                           {commitTimestamp: prepareTimestamp})));

    unpauseMigrateAtStep(st.shard1, migrateStepNames.catchup);

    joinMoveChunk();

    class ArrayCursor {
        constructor(arr) {
            this.i = 0;
            this.arr = arr;
        }

        hasNext() {
            return this.i < this.arr.length;
        }

        next() {
            return this.arr[this.i++];
        }
    }

    const expected = [
        {_id: 1, x: 5, note: "move into chunk range being migrated"},
        {_id: 2, x: -10, note: "keep out of chunk range being migrated"},
        {_id: 3, x: -20, note: "move out of chunk range being migrated"},
        {_id: 4, x: 500, note: "keep in chunk range being migrated"},
        {_id: 5, x: 5.01, note: "move into chunk range being migrated"},
        {_id: 6, x: -10.01, note: "keep out of chunk range being migrated"},
        {_id: 7, x: -20.01, note: "move out of chunk range being migrated"},
        {_id: 8, x: 500.01, note: "keep in chunk range being migrated"},
    ];

    let result = collection.find().sort({_id: 1, x: 1}).toArray();
    assert.sameMembers(expected, result);

    st.stop();
};

runTest(TestMode.kBasic);
runTest(TestMode.kWithStepUp);
runTest(TestMode.kWithRestart);

MongoRunner.stopMongod(staticMongod);
})();
