/*
 * Test that the explain command supports findAndModify when talking to a mongos
 * and the collection is sharded.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let collName = "explain_find_and_modify";

// Create a cluster with 2 shards.
let st = new ShardingTest({shards: 2});

let testDB = st.s.getDB("test");
let shardKey = {a: 1};

// Use "st.shard0.shardName" as the primary shard.
assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

// Create a collection with an index on the intended shard key.
let shardedColl = testDB.getCollection(collName);
shardedColl.drop();
assert.commandWorked(testDB.createCollection(collName));
assert.commandWorked(shardedColl.createIndex(shardKey));

assert.commandWorked(testDB.adminCommand({shardCollection: shardedColl.getFullName(), key: shardKey}));

// Split and move the chunks so that
//   chunk { "a" : { "$minKey" : 1 } } -->> { "a" : 10 }                is on
//   st.shard0.shardName
//   chunk { "a" : 10 }                -->> { "a" : { "$maxKey" : 1 } } is on
//   st.shard1.shardName
assert.commandWorked(testDB.adminCommand({split: shardedColl.getFullName(), middle: {a: 10}}));
assert.commandWorked(
    testDB.adminCommand({moveChunk: shardedColl.getFullName(), find: {a: 10}, to: st.shard1.shardName}),
);

let res;

// Sharded updateOne that does not target a single shard can now be executed with a two phase
// write protocol that will target at most 1 matching document.
res = assert.commandWorked(
    testDB.runCommand({explain: {findAndModify: collName, query: {b: 1}, remove: true}, verbosity: "queryPlanner"}),
);

assert(res.queryPlanner);
assert(!res.executionStats);
assert.eq(res.queryPlanner.winningPlan.stage, "SHARD_WRITE");
assert.eq(res.queryPlanner.winningPlan.inputStage.winningPlan.stage, "SHARD_MERGE");

res = assert.commandWorked(
    testDB.runCommand({
        explain: {
            findAndModify: collName,
            query: {a: {$gt: 5}},
            update: {$inc: {b: 7}},
        },
        verbosity: "allPlansExecution",
    }),
);

assert(res.queryPlanner);
assert(res.executionStats);
assert.eq(res.queryPlanner.winningPlan.stage, "SHARD_WRITE");
assert.eq(res.queryPlanner.winningPlan.inputStage.winningPlan.stage, "SHARD_MERGE");
assert.eq(res.executionStats.executionStages.stage, "SHARD_WRITE");
assert.eq(res.executionStats.inputStage.executionStages.stage, "SHARD_MERGE");

// Asserts that the explain command ran on the specified shard and used the given stage
// for performing the findAndModify command.
function assertExplainResult(explainOut, outerKey, innerKey, shardName, expectedStage) {
    assert(explainOut.hasOwnProperty(outerKey));
    assert(explainOut[outerKey].hasOwnProperty(innerKey));

    let shardStage = explainOut[outerKey][innerKey];
    assert.eq("SINGLE_SHARD", shardStage.stage);
    assert.eq(1, shardStage.shards.length);
    assert.eq(shardName, shardStage.shards[0].shardName);
    assert.eq(expectedStage, shardStage.shards[0][innerKey].stage);
}

// Test that the explain command is routed to "st.shard0.shardName" when targeting the lower
// chunk range.
res = testDB.runCommand({
    explain: {findAndModify: collName, query: {a: 0}, update: {$inc: {b: 7}}, upsert: true},
    verbosity: "queryPlanner",
});
assert.commandWorked(res);
assertExplainResult(res, "queryPlanner", "winningPlan", st.shard0.shardName, "UPDATE");

// Test that the explain command is routed to "st.shard1.shardName" when targeting the higher
// chunk range.
res = testDB.runCommand({
    explain: {findAndModify: collName, query: {a: 20, c: 5}, remove: true},
    verbosity: "executionStats",
});
assert.commandWorked(res);
assertExplainResult(res, "executionStats", "executionStages", st.shard1.shardName, "DELETE");

st.stop();
