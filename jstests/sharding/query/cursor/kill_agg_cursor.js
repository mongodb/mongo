/**
 * Test that explicitly killing an aggregation cursor on a sharded collection works correctly.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_getmore,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const testDB = st.s.getDB(jsTestName());
const collName = jsTestName();
const coll = testDB[collName];

// Shard the collection on _id.
st.shardColl(coll, {_id: 1} /* key */, {_id: 0} /* split */, {_id: 0} /* move */, testDB.getName());

const docs = [];
for (let i = -50; i < 50; i++) {
    docs.push({_id: i, value: i % 10, data: "x".repeat(100)});
}
assert.commandWorked(coll.insertMany(docs));

const shard0Count = st.shard0.getDB(testDB.getName())[collName].count();
const shard1Count = st.shard1.getDB(testDB.getName())[collName].count();
assert.gt(shard0Count, 0, "Expected documents on shard0");
assert.gt(shard1Count, 0, "Expected documents on shard1");

const batchSize = 2;

// Run an aggregation with $group that will return multiple batches.
jsTestLog("Running aggregation with $group...");
const aggRes = assert.commandWorked(testDB.runCommand({
    aggregate: collName,
    pipeline:
        [{$group: {_id: "$value", count: {$sum: 1}, docs: {$push: "$data"}}}, {$sort: {_id: 1}}],
    cursor: {batchSize: batchSize}
}));

assert.gt(aggRes.cursor.id, 0, "Expected a non-zero cursor id");
jsTestLog("Got cursor with id: " + aggRes.cursor.id);

jsTestLog("Issuing getMore...");
assert.commandWorked(
    testDB.runCommand({getMore: aggRes.cursor.id, collection: collName, batchSize: batchSize}));

jsTestLog("Killing cursor with id: " + aggRes.cursor.id);
const killRes =
    assert.commandWorked(testDB.runCommand({killCursors: collName, cursors: [aggRes.cursor.id]}));
assert.eq(1, killRes.cursorsKilled.length, "Expected cursor to be killed");

jsTestLog("A getMore on killed cursor should fail...");
assert.commandFailedWithCode(
    testDB.runCommand({getMore: aggRes.cursor.id, collection: collName, batchSize: batchSize}),
    ErrorCodes.CursorNotFound);

st.stop();
