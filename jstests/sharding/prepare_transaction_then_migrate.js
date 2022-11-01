/**
 * Test that a migration will:
 * 1. Ignore multi-statement transaction prepare conflicts in the clone phase, and
 * 2. Pick up the changes for prepared transactions in the transfer mods phase.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load('jstests/libs/chunk_manipulation_util.js');
load('jstests/sharding/libs/create_sharded_collection_util.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "user";

const staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

let runTest = function(withStepUp) {
    const st = new ShardingTest({shards: {rs0: {nodes: withStepUp ? 2 : 1}, rs1: {nodes: 1}}});
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

    if (withStepUp) {
        st.rs0.stepUp(st.rs0.getSecondary());
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

    const expected = new ArrayCursor([
        {_id: 1, x: 5, note: "move into chunk range being migrated"},
        {_id: 2, x: -10, note: "keep out of chunk range being migrated"},
        {_id: 3, x: -20, note: "move out of chunk range being migrated"},
        {_id: 4, x: 500, note: "keep in chunk range being migrated"},
        {_id: 5, x: 5.01, note: "move into chunk range being migrated"},
        {_id: 6, x: -10.01, note: "keep out of chunk range being migrated"},
        {_id: 7, x: -20.01, note: "move out of chunk range being migrated"},
        {_id: 8, x: 500.01, note: "keep in chunk range being migrated"},
    ]);

    const diff = ((diff) => {
        return {
            docsWithDifferentContents: diff.docsWithDifferentContents.map(
                ({first, second}) => ({expected: first, actual: second})),
            docsExtraAfterMigration: diff.docsMissingOnFirst,
            docsMissingAfterMigration: diff.docsMissingOnSecond,
        };
    })(DataConsistencyChecker.getDiff(expected, collection.find().sort({_id: 1, x: 1})));

    assert.eq(diff, {
        docsWithDifferentContents: [],
        docsExtraAfterMigration: [],
        docsMissingAfterMigration: [],
    });

    st.stop();
};

runTest(false);
// TODO: SERVER-71219 Enable test after fixing.
// runTest(true);

MongoRunner.stopMongod(staticMongod);
})();
