// Tests various combinations of $lookup and $unionWith to ensure that referencing a sharded
// collection within $lookup is not allowed.
(function() {
"use strict";

const testName = "unionWith_lookup";
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

const mongosDB = st.s0.getDB(testName);
assert.commandWorked(mongosDB.dropDatabase());
const baseColl = mongosDB[testName + "_local"];
const unionColl = mongosDB[testName + "_union"];
const lookupColl = mongosDB[testName + "_lookup"];

// Ensure that shard0 is the primary shard.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

// Seed the involved collections.
assert.commandWorked(baseColl.insert({_id: 0}));
assert.commandWorked(unionColl.insert({_id: 1}));
assert.commandWorked(lookupColl.insert({_id: 0, foreignField: 1, data: "lookup_data"}));

let expectedResults =
    [{_id: 0}, {_id: 1, result: [{_id: 0, foreignField: 1, data: "lookup_data"}]}];

// Test that $lookup within a $unionWith sub-pipeline works when all collections are unsharded.
const lookupUnderUnionPipeline = [{
    $unionWith: {
        coll: unionColl.getName(), 
        pipeline: [{
            $lookup: {
                from: lookupColl.getName(), 
                localField: "_id", 
                foreignField: "foreignField", 
                as: "result"
            }
        }]
    }
}];
assert.eq(baseColl.aggregate(lookupUnderUnionPipeline).toArray(), expectedResults);

// Test that $unionWith within a $lookup sub-pipeline works when all collections are unsharded.
const unionUnderLookupPipeline = [{
    $lookup:
        {from: lookupColl.getName(), pipeline: [{$unionWith: unionColl.getName()}], as: "result"}
}];
expectedResults = [{_id: 0, result: [{_id: 0, foreignField: 1, data: "lookup_data"}, {_id: 1}]}];
assert.eq(baseColl.aggregate(unionUnderLookupPipeline).toArray(), expectedResults);

// Shard the base and union collection.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: baseColl.getFullName(), key: {_id: 1}}));
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: unionColl.getFullName(), key: {_id: 1}}));

// Test that a $lookup with a sub-pipeline containing a $unionWith on a sharded collection works.
assert.eq(baseColl.aggregate(unionUnderLookupPipeline).toArray(), expectedResults);

// Test that $lookup from an unsharded collection within a $unionWith sub-pipeline works even if
// the union collection is sharded.
expectedResults = [{_id: 0}, {_id: 1, result: [{_id: 0, foreignField: 1, data: "lookup_data"}]}];
assert.eq(baseColl.aggregate(lookupUnderUnionPipeline).toArray(), expectedResults);

// Shard the lookup collection.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: lookupColl.getFullName(), key: {_id: 1}}));

// Test that $lookup from a sharded collection is allowed within a $unionWith pipeline.
assert.eq(baseColl.aggregate(lookupUnderUnionPipeline).toArray(), expectedResults);

// Test that a deeply nested $lookup from a sharded collection works.
assert.commandWorked(mongosDB.unshardedLookupColl.insert({_id: 0}));
assert.commandWorked(mongosDB.unshardedUnionColl.insert({_id: 1}));
const deepLookupPipeline = [{
    $lookup: {
        from: "unshardedLookupColl", 
        pipeline: [{
            $unionWith: {
                coll: "unshardedUnionColl", 
                pipeline: [{
                    $lookup: {
                        from: lookupColl.getName(), 
                        localField: "_id", 
                        foreignField: "foreignField", 
                        as: "result"
                    }
                }]
            }
        }], 
        as: "result"
    }
}];

expectedResults = [
    {_id: 0, result: [{_id: 0}, {_id: 1, result: [{_id: 0, foreignField: 1, data: "lookup_data"}]}]}
];
assert.eq(baseColl.aggregate(deepLookupPipeline).toArray(), expectedResults);
st.stop();
}());
