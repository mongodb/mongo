/**
 * Verify the recipient shard continues to run the _transferMods command against the donor shard
 * primary until it receives an empty _transferMods batch after the kCommitStart recipient state was
 * reached. In particular, a batch of changes unrelated to the chunk migration must not cause the
 * recipient shard to stop running the _transferMods command.
 *
 * @tags: [uses_transactions]
 */
import {
    withRetryOnTransientTxnErrorIncrementTxnNum
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {
    moveChunkParallel,
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const staticMongod = MongoRunner.runMongod({});  // Mongod used for startParallelOps().
const st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}});

const dbName = "test";
const collName = "transfer_mods_large_batches";
const collection = st.s.getDB(dbName).getCollection(collName);

CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 1000}, shard: st.shard0.shardName},
    {min: {x: 1000}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

function insertLargeDocsInTransaction(collection, docIds, shardKey) {
    const lsid = {id: UUID()};
    const txnNumber = 0;
    const largeStr = "x".repeat(9 * 1024 * 1024);
    withRetryOnTransientTxnErrorIncrementTxnNum(txnNumber, (txnNum) => {
        for (let i = 0; i < docIds.length; ++i) {
            const docToInsert = {_id: docIds[i]._id};
            Object.assign(docToInsert, shardKey);
            docToInsert.note = "large document to force separate _transferMods call";
            docToInsert.padding = largeStr;

            const commandObj = {
                documents: [docToInsert],
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                autocommit: false
            };

            if (i === 0) {
                commandObj.startTransaction = true;
            }

            assert.commandWorked(collection.runCommand("insert", commandObj));
        }

        assert.commandWorked(collection.getDB().adminCommand(
            {commitTransaction: 1, lsid: lsid, txnNumber: NumberLong(txnNum), autocommit: false}));
    });
}

assert.commandWorked(collection.insert([
    {_id: 1, x: -2, note: "keep out of chunk range being migrated"},
    {_id: 2, x: 100, note: "keep in chunk range being migrated"},
]));

pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
const fp = configureFailPoint(st.shard1.rs.getPrimary(), "migrateThreadHangAfterSteadyTransition");

const joinMoveChunk = moveChunkParallel(
    staticMongod, st.s.host, {x: 1}, undefined, collection.getFullName(), st.shard1.shardName);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);
insertLargeDocsInTransaction(collection, [{_id: 3}, {_id: 4}], {x: -1000});
assert.commandWorked(
    collection.insert({_id: 5, x: 1, note: "inserted into range after large _transferMods"}));

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

// The kCommitStart state isn't a separate "step" of the chunk migration procedure on the
// recipient shard. We therefore cannot use the waitForMigrateStep() helper to wait for the
// _recvChunkCommit command to have been received by the recipient shard. The problematic
// behavior of the recipient shard finishing its catch up too early only manifests after the
// _recvChunkCommit command has been received by the recipient shard.
assert.soon(() => {
    const res = assert.commandWorked(fp.conn.adminCommand({_recvChunkStatus: 1}));
    return res.state !== "steady";
});

fp.off();
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
    {_id: 1, x: -2, note: "keep out of chunk range being migrated"},
    {_id: 2, x: 100, note: "keep in chunk range being migrated"},
    {_id: 3, x: -1000, note: "large document to force separate _transferMods call"},
    {_id: 4, x: -1000, note: "large document to force separate _transferMods call"},
    {_id: 5, x: 1, note: "inserted into range after large _transferMods"},
]);

const diff = ((diff) => {
    return {
        docsWithDifferentContents: diff.docsWithDifferentContents.map(
            ({first, second}) => ({expected: first, actual: second})),
        docsExtraAfterMigration: diff.docsMissingOnFirst,
        docsMissingAfterMigration: diff.docsMissingOnSecond,
    };
})(
    DataConsistencyChecker.getDiff(
        expected, collection.find({}, {_id: 1, x: 1, note: 1}).sort({_id: 1, x: 1})));

assert.eq(diff, {
    docsWithDifferentContents: [],
    docsExtraAfterMigration: [],
    docsMissingAfterMigration: [],
});

st.stop();
MongoRunner.stopMongod(staticMongod);
