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
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shard: 1});

let dbName = 'test';
let collName = 'testColl';
let collNs = dbName + '.' + collName;

let db1a = st.s0.getDB(dbName);
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

let aggResult = shard0db.runCommand({
    aggregate: collName,
    pipeline: [{$match: {"a": {$gt: 0}}}],
    cursor: {batchSize: 0},
    lsid: lsid,
    txnNumber: NumberLong(txnNumber),
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
    txnNumber: NumberLong(txnNumber),
    startOrContinueTransaction: true,
    autocommit: false,
    readConcern: readConcern,
});
assert.commandWorked(getMoreResult);

st.stop();
