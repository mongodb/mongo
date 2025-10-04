/**
 * Tests that session information are properly transferred to the destination shard while
 * new writes are being sent to the source shard.
 */

/**
 * Test outline:
 * 1. Pause migration.
 * 2. Perform writes and allow it to be capture via OpObserver
 * 3. Unpause migration.
 * 4. Retry writes and confirm that writes are not duplicated.
 */

import {
    moveChunkParallel,
    moveChunkStepNames,
    pauseMoveChunkAtStep,
    unpauseMoveChunkAtStep,
    waitForMoveChunkStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let staticMongod = MongoRunner.runMongod({}); // For startParallelOps.

let st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}});
st.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName});
st.adminCommand({shardCollection: "test.user", key: {x: 1}});
assert.commandWorked(st.s.adminCommand({split: "test.user", middle: {x: 0}}));

pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
let joinMoveChunk = moveChunkParallel(staticMongod, st.s.host, {x: 0}, null, "test.user", st.shard1.shardName);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

const insertCmd = {
    insert: "user",
    documents: [
        // For findAndModify not touching chunk being migrated.
        {x: -30},
        // For changing doc to become owned by chunk being migrated.
        {x: -20},
        {x: -20},
        // For basic insert.
        {x: 10},
        // For changing doc to become owned by another chunk not being migrated.
        {x: 20},
        {x: 20},
        // For basic findAndModify.
        {x: 30},
    ],
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(34),
};

let testDB = st.getDB("test");
const insertResult = assert.commandWorked(testDB.runCommand(insertCmd));

const findAndModCmd = {
    findAndModify: "user",
    query: {x: 30},
    update: {$inc: {y: 1}},
    new: true,
    upsert: true,
    lsid: {id: UUID()},
    txnNumber: NumberLong(37),
};

const findAndModifyResult = assert.commandWorked(testDB.runCommand(findAndModCmd));

const changeDocToChunkNotMigrated = {
    findAndModify: "user",
    query: {x: 20},
    update: {$set: {x: -120}, $inc: {y: 1}},
    new: false,
    upsert: true,
    lsid: {id: UUID()},
    txnNumber: NumberLong(37),
};

const changeDocToNotMigratedResult = assert.commandWorked(testDB.runCommand(changeDocToChunkNotMigrated));

const changeDocToChunkMigrated = {
    findAndModify: "user",
    query: {x: -20},
    update: {$set: {x: 120}, $inc: {y: 1}},
    new: false,
    upsert: true,
    lsid: {id: UUID()},
    txnNumber: NumberLong(37),
};

const changeDocToMigratedResult = assert.commandWorked(testDB.runCommand(changeDocToChunkMigrated));

const findAndModifyNotMigrated = {
    findAndModify: "user",
    query: {x: -30},
    update: {$inc: {y: 1}},
    new: false,
    upsert: true,
    lsid: {id: UUID()},
    txnNumber: NumberLong(37),
};

const findAndModifyNotMigratedResult = assert.commandWorked(testDB.runCommand(findAndModifyNotMigrated));

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
joinMoveChunk();

///////////////////////////////////////////////////////////////////////////////////////////////
// Retry phase

let insertRetryResult = assert.commandWorked(testDB.runCommand(insertCmd));

assert.eq(insertResult.ok, insertRetryResult.ok);
assert.eq(insertResult.n, insertRetryResult.n);
assert.eq(insertResult.writeErrors, insertRetryResult.writeErrors);
assert.eq(insertResult.writeConcernErrors, insertRetryResult.writeConcernErrors);

assert.eq(1, testDB.user.find({x: 10}).itcount());
assert.eq(1, testDB.user.find({x: 30}).itcount());

let findAndModifyRetryResult = assert.commandWorked(testDB.runCommand(findAndModCmd));

assert.eq(findAndModifyResult.ok, findAndModifyRetryResult.ok);
assert.eq(findAndModifyResult.value, findAndModifyRetryResult.value);
assert.eq(findAndModifyResult.lastErrorObject, findAndModifyRetryResult.lastErrorObject);

assert.eq(1, testDB.user.findOne({x: 30}).y);

let changeDocToNotMigratedRetryResult = assert.commandWorked(testDB.runCommand(changeDocToChunkNotMigrated));

assert.eq(changeDocToNotMigratedResult.ok, changeDocToNotMigratedRetryResult.ok);
assert.eq(changeDocToNotMigratedResult.value, changeDocToNotMigratedRetryResult.value);
assert.eq(changeDocToNotMigratedResult.lastErrorObject, changeDocToNotMigratedRetryResult.lastErrorObject);

assert.eq(1, testDB.user.find({x: -120}).itcount());

let changeDocToMigratedRetryResult = assert.commandWorked(testDB.runCommand(changeDocToChunkMigrated));

assert.eq(changeDocToMigratedResult.ok, changeDocToMigratedRetryResult.ok);
assert.eq(changeDocToMigratedResult.value, changeDocToMigratedRetryResult.value);
assert.eq(changeDocToMigratedResult.lastErrorObject, changeDocToMigratedRetryResult.lastErrorObject);

assert.eq(1, testDB.user.find({x: 120}).itcount());

let findAndModifyNotMigratedRetryResult = assert.commandWorked(testDB.runCommand(findAndModifyNotMigrated));

assert.eq(findAndModifyNotMigratedResult.ok, findAndModifyNotMigratedRetryResult.ok);
assert.eq(findAndModifyNotMigratedResult.value, findAndModifyNotMigratedRetryResult.value);
assert.eq(findAndModifyNotMigratedResult.lastErrorObject, findAndModifyNotMigratedRetryResult.lastErrorObject);

assert.eq(1, testDB.user.findOne({x: -30}).y);

st.stop();

MongoRunner.stopMongod(staticMongod);
