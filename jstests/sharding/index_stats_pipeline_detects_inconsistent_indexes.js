/**
 * Test to demonstrate usage of $indexStats in an aggregation pipeline to detect inconsistent
 * indexes in a sharded cluster.
 * @tags: [requires_fcv_44]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For documentEq.

// This test deliberately creates indexes in an inconsistent state.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const testName = "detect_inconsistent_indexes";
const st = new ShardingTest({shards: 3});
const dbName = "test";

// Pipeline used to detect inconsistent indexes.
const pipeline = [
    // Get indexes and the shards that they belong to.
    {$indexStats: {}},
    // Attach a list of all shards which reported indexes to each document from $indexStats.
    {$group: {_id: null, indexDoc: {$push: "$$ROOT"}, allShards: {$addToSet: "$shard"}}},
    // Unwind the generated array back into an array of index documents.
    {$unwind: "$indexDoc"},
    // Group by index name.
    {
        $group: {
            "_id": "$indexDoc.name",
            "shards": {$push: "$indexDoc.shard"},
            // Index specs are stored as BSON objects and may have fields in any order, but there is
            // currently no way to cleanly compare objects ignoring field order in an aggregation,
            // so convert each spec into an array of its properties instead.
            "specs": {$push: {$objectToArray: {$ifNull: ["$indexDoc.spec", {}]}}},
            "allShards": {$first: "$allShards"}
        }
    },
    // Compute which indexes are not present on all targeted shards and which index spec properties
    // aren't the same across all shards.
    {
        $project: {
            missingFromShards: {$setDifference: ["$allShards", "$shards"]},
            inconsistentProperties: {
                 $setDifference: [
                     {$reduce: {
                         input: "$specs",
                         initialValue: {$arrayElemAt: ["$specs", 0]},
                         in: {$setUnion: ["$$value", "$$this"]}}},
                     {$reduce: {
                         input: "$specs",
                         initialValue: {$arrayElemAt: ["$specs", 0]},
                         in: {$setIntersection: ["$$value", "$$this"]}}}
                 ]
             }
        }
    },
    // Only return output that indicates an index was inconsistent, i.e. either a shard was missing
    // an index or a property on at least one shard was not the same on all others.
    {
        $match: {
            $expr:
                {$or: [
                    {$gt: [{$size: "$missingFromShards"}, 0]},
                    {$gt: [{$size: "$inconsistentProperties"}, 0]},
                ]
            }
        }
    },
    // Output relevant fields.
    {$project: {_id: 0, indexName: "$$ROOT._id", inconsistentProperties: 1, missingFromShards: 1}}
];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

function shardCollectionWithChunkOnEachShard(collName) {
    const ns = dbName + "." + collName;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 100}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 100}, to: st.shard2.shardName}));
}

//
// Cases with consistent indexes.
//

(() => {
    jsTestLog("No indexes on any shard...");

    const collName = "noIndexes";
    shardCollectionWithChunkOnEachShard(collName);

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

(() => {
    jsTestLog("Index on each shard...");

    const collName = "indexOnEachShard";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.s.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

(() => {
    jsTestLog("Index on each shard with chunks...");

    const collName = "indexOnEachShardWithChunks";
    shardCollectionWithChunkOnEachShard(collName);
    // Move the chunk off shard2.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: dbName + "." + collName, find: {_id: 100}, to: st.shard1.shardName}));

    assert.commandWorked(st.s.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

(() => {
    jsTestLog("Index on each shard with chunks not on primary shard...");

    const collName = "indexOnEachShardWithChunksNotPrimary";
    shardCollectionWithChunkOnEachShard(collName);
    // Move the chunk off the primary shard.
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: dbName + "." + collName, find: {_id: -1}, to: st.shard1.shardName}));

    assert.commandWorked(st.s.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

(() => {
    jsTestLog("Index on each shard with expireAfterSeconds...");

    const collName = "indexOnEachShardWithTTL";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(
        st.s.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 101}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

(() => {
    jsTestLog("Same options but in different orders...");

    const collName = "sameOptionsDiffOrders";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex(
        {_id: 1, x: 1},
        {collation: {locale: "fr"}, partialFilterExpression: {x: {$gt: 50}}, unique: true}));
    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex(
        {_id: 1, x: 1},
        {partialFilterExpression: {x: {$gt: 50}}, unique: true, collation: {locale: "fr"}}));
    assert.commandWorked(st.shard2.getDB(dbName)[collName].createIndex(
        {_id: 1, x: 1},
        {unique: true, partialFilterExpression: {x: {$gt: 50}}, collation: {locale: "fr"}}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 0, tojson(res));
})();

//
// Cases with inconsistent indexes.
//

(() => {
    jsTestLog("Not on one shard...");

    const collName = "notOnOneShard";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex({x: 1}));
    assert.commandWorked(st.shard2.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [st.shard0.shardName],
               inconsistentProperties: [],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Not on two shards...");

    const collName = "notOnTwoShards";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [st.shard0.shardName, st.shard2.shardName],
               inconsistentProperties: [],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Different keys...");

    const collName = "differentKeys";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex({x: 1}, {name: "diffKeys"}));
    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex({y: 1}, {name: "diffKeys"}));
    assert.commandWorked(st.shard2.getDB(dbName)[collName].createIndex({z: 1}, {name: "diffKeys"}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "diffKeys",
               missingFromShards: [],
               inconsistentProperties:
                   [{k: "key", v: {x: 1}}, {k: "key", v: {y: 1}}, {k: "key", v: {z: 1}}],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Different property...");

    const collName = "differentTTL";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(
        st.shard0.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 105}));
    assert.commandWorked(
        st.shard1.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 106}));
    assert.commandWorked(
        st.shard2.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 107}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [],
               inconsistentProperties: [
                   {k: "expireAfterSeconds", v: 105},
                   {k: "expireAfterSeconds", v: 106},
                   {k: "expireAfterSeconds", v: 107}
               ],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Missing property...");

    const collName = "missingTTL";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(
        st.shard0.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 105}));
    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex({x: 1}));
    assert.commandWorked(st.shard2.getDB(dbName)[collName].createIndex({x: 1}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [],
               inconsistentProperties: [{k: "expireAfterSeconds", v: 105}],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Multiple different parameters...");

    const collName = "multipleDifferent";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex(
        {x: 1}, {expireAfterSeconds: 100, partialFilterExpression: {x: {$gt: 50}}}));
    assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex(
        {x: 1}, {expireAfterSeconds: 101, partialFilterExpression: {x: {$gt: 51}}}));
    assert.commandWorked(st.shard2.getDB(dbName)[collName].createIndex(
        {x: 1}, {expireAfterSeconds: 102, partialFilterExpression: {x: {$gt: 52}}}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [],
               inconsistentProperties: [
                   {k: "expireAfterSeconds", v: 100},
                   {k: "expireAfterSeconds", v: 101},
                   {k: "expireAfterSeconds", v: 102},
                   {k: "partialFilterExpression", v: {x: {$gt: 50}}},
                   {k: "partialFilterExpression", v: {x: {$gt: 51}}},
                   {k: "partialFilterExpression", v: {x: {$gt: 52}}}
               ],
           }),
           tojson(res));
})();

(() => {
    jsTestLog("Missing and different parameters and missing from one shard...");

    const collName = "missingDifferentParametersAndMissingFromShard";
    shardCollectionWithChunkOnEachShard(collName);

    assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex(
        {x: 1}, {expireAfterSeconds: 101, partialFilterExpression: {x: {$gt: 50}}}));
    assert.commandWorked(
        st.shard1.getDB(dbName)[collName].createIndex({x: 1}, {expireAfterSeconds: 100}));

    const res = st.s.getDB(dbName)[collName].aggregate(pipeline).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert(documentEq(res[0], {
               indexName: "x_1",
               missingFromShards: [st.shard2.shardName],
               inconsistentProperties: [
                   {k: "expireAfterSeconds", v: 100},
                   {k: "expireAfterSeconds", v: 101},
                   {k: "partialFilterExpression", v: {x: {$gt: 50}}},
               ],
           }),
           tojson(res));
})();

st.stop();
})();
