/**
 * Test that getMore request with startOrContinueTransaction and readConcernArgs with
 * afterClusterTime is processed without error.
 *
 * @tags: [
 *      requires_fcv_80,
 *      requires_majority_read_concern,
 *      uses_transactions,
 * ]
 */
import {
    withRetryOnTransientTxnErrorIncrementTxnNum
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shard: 1});

let dbName = 'test';
let collName = 'testColl';

assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));

let lsid = {id: UUID()};
let txnNumber = 0;
let stmtId = 0;
let shard0db = st.getPrimaryShard(dbName).getDB(dbName);

let insertResult =
    shard0db.runCommand({insert: collName, documents: [{a: 1}], writeConcern: {w: "majority"}});
assert.commandWorked(insertResult);
let clusterTime = shard0db.getSession().getOperationTime();

let readConcern = {level: "majority", afterClusterTime: clusterTime};

withRetryOnTransientTxnErrorIncrementTxnNum(txnNumber, (txnNum) => {
    let aggResult = shard0db.runCommand({
        aggregate: collName,
        pipeline: [{$match: {"a": {$gt: 0}}}],
        cursor: {batchSize: 0},
        lsid: lsid,
        txnNumber: NumberLong(txnNum),
        stmtId: NumberInt(stmtId),
        startTransaction: true,
        autocommit: false,
        readConcern: readConcern,
    });
    assert.commandWorked(aggResult);

    let getMoreResult = shard0db.runCommand({
        getMore: aggResult.cursor.id,
        collection: collName,
        batchSize: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNum),
        startOrContinueTransaction: true,
        autocommit: false,
        readConcern: readConcern,
    });
    assert.commandWorked(getMoreResult);

    assert.commandWorked(shard0db.adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNum),
        autocommit: false,
    }));
});

st.stop();
