/**
 * Test that $unionWith works when unioning unsharded with sharded collections, and vice versa.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   requires_replication,
 *   requires_sharding,
 *   incompatible_with_lockfreereads, // SERVER-51319
 * ]
 */

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // arrayEq

function getDocsFromCollection(collObj) {
    return collObj.find().toArray();
}
function checkResults(resObj, expectedResult) {
    assert(arrayEq(resObj.cursor.firstBatch, expectedResult),
           "Expected:\n" + tojson(expectedResult) + "Got:\n" + tojson(resObj.cursor.firstBatch));
}
const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
const mongos = st.s;
const dbName = jsTestName();
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const testDB = mongos.getDB(dbName);
const shardedCollOne = testDB.shardedCollOne;
shardedCollOne.drop();
const shardedCollTwo = testDB.shardedCollTwo;
shardedCollTwo.drop();
const unshardedCollOne = testDB.unshardedCollOne;
unshardedCollOne.drop();
const unshardedCollTwo = testDB.unshardedCollTwo;
unshardedCollTwo.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(shardedCollOne.insert({val: i}));
    assert.commandWorked(shardedCollTwo.insert({val: i * 2}));
    assert.commandWorked(unshardedCollOne.insert({val: i * 3}));
    assert.commandWorked(unshardedCollTwo.insert({val: i * 4}));
}
assert.commandWorked(
    mongos.adminCommand({shardCollection: shardedCollOne.getFullName(), key: {_id: 1}}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: shardedCollTwo.getFullName(), key: {_id: 1}}));

// Run each test against both the primary and non-primary shards.
// Make sure the primary is always shard0.
st.ensurePrimaryShard(dbName, st.shard0.shardName);
const shardNames = [st.shard0.shardName, st.shard1.shardName];
shardNames.forEach(function(shardName) {
    jsTestLog("Testing with docs on " + shardName);
    testDB.adminCommand({moveChunk: shardedCollOne.getFullName(), find: {_id: 0}, to: shardName});
    testDB.adminCommand({moveChunk: shardedCollTwo.getFullName(), find: {_id: 0}, to: shardName});
    // Test one sharded and one unsharded collection.
    let resSet =
        getDocsFromCollection(shardedCollOne).concat(getDocsFromCollection(unshardedCollOne));
    let resObj = assert.commandWorked(testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{$unionWith: unshardedCollOne.getName()}],
        cursor: {}
    }));
    checkResults(resObj, resSet);
    resObj = assert.commandWorked(testDB.runCommand({
        aggregate: unshardedCollOne.getName(),
        pipeline: [{$unionWith: shardedCollOne.getName()}],
        cursor: {}
    }));
    checkResults(resObj, resSet);

    // Test a union of two sharded collections and one unsharded collection.
    resSet = getDocsFromCollection(shardedCollOne)
                 .concat(getDocsFromCollection(unshardedCollOne))
                 .concat(getDocsFromCollection(shardedCollTwo));
    resObj = assert.commandWorked(testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{
            $unionWith: {
                coll: unshardedCollOne.getName(),
                pipeline: [{$unionWith: shardedCollTwo.getName()}]
            }
        }],
        cursor: {}
    }));
    checkResults(resObj, resSet);
    // Test a union of two unsharded collections and one sharded collection.
    resSet = getDocsFromCollection(unshardedCollOne)
                 .concat(getDocsFromCollection(shardedCollOne))
                 .concat(getDocsFromCollection(unshardedCollTwo));
    resObj = assert.commandWorked(testDB.runCommand({
        aggregate: unshardedCollOne.getName(),
        pipeline: [{
            $unionWith: {
                coll: shardedCollOne.getName(),
                pipeline: [{$unionWith: unshardedCollTwo.getName()}]
            }
        }],
        cursor: {}
    }));
    checkResults(resObj, resSet);

    // Test a union of two sharded collections when the documents are on different shards.
    jsTestLog("Testing with docs on two different shards");
    testDB.adminCommand({
        moveChunk: shardedCollTwo.getFullName(),
        find: {_id: 0},
        to: st.shard0.shardName == shardName ? st.shard1.shardName : st.shard0.shardName
    });
    resSet = getDocsFromCollection(shardedCollOne).concat(getDocsFromCollection(shardedCollTwo));
    resObj = assert.commandWorked(testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{$unionWith: shardedCollTwo.getName()}],
        cursor: {}
    }));
    checkResults(resObj, resSet);
    // Test a union of two sharded collections on different shards with an additional unsharded
    // collection.
    resSet = resSet.concat(getDocsFromCollection(unshardedCollOne));
    resObj = assert.commandWorked(testDB.runCommand({
        aggregate: shardedCollOne.getName(),
        pipeline: [{
            $unionWith: {
                coll: unshardedCollOne.getName(),
                pipeline: [{$unionWith: shardedCollTwo.getName()}]
            }
        }],
        cursor: {}
    }));
    checkResults(resObj, resSet);
});
st.stop();
})();
