/**
 * Test to demonstrate usage of $indexStats in an aggregation pipeline to detect inconsistent
 * indexes in a sharded cluster.
 * @tags: [requires_fcv_44]
 */

(function() {
"use strict";

// This test deliberately creates indexes in an inconsistent state.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

const testName = "detect_inconsistent_indexes";
const st = new ShardingTest({shards: 3});
const dbName = "test";
const testDB = st.s.getDB(dbName);
const coll = testDB[testName];

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
            // This constructs an array of unique index specs, independent of field ordering.
            //
            // Since $setUnion uses a sorted set internally, each spec can be reordered to
            // have its keys ordered. These ordered index specs can now be compared for
            // equality, so $addToSet will return an array of distinct index specs.
            "specs": {$addToSet: {$arrayToObject: {$setUnion: {$objectToArray: "$indexDoc.spec"}}}},
            "allShards": {$first: "$allShards"}
        }
    },
    // Compute set difference of shard names.
    {$addFields: {"missingFromShards": {$setDifference: ["$allShards", "$shards"]}}},
    // Only report indexes which either are missing from certain shards or have multiple specs
    // defined for the same index name.
    {
        $match: {
            $expr: {$or: [{$gt: [{$size: "$missingFromShards"}, 0]}, {$gt: [{$size: "$specs"}, 1]}]}
        }
    },
    // Output relevant fields.
    {$project: {_id: 0, indexName: "$$ROOT._id", specs: 1, missingFromShards: 1}}
];

assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));

st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split collection and moveChunks such that each shard will have exactly one chunk.
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 25}}));
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 50}}));

assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

assert.commandWorked(
    st.s.adminCommand({moveChunk: coll.getFullName(), find: {_id: 50}, to: st.shard2.shardName}));

const bulkOp = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulkOp.insert({_id: i, a: i, b: i * 2, c: i / 2, d: i, e: i * 3});
}
assert.commandWorked(bulkOp.execute());

// Index we expect to be on all shards
const sharedIndex = {
    a: 1
};

// Index we expect to missing on exactly one shard (shard0).
const indexMissingFromOneShard = {
    b: 1
};

// Index we expect to missing on exactly two shards (shard1 and shard2).
const indexMissingFromTwoShards = {
    c: 1
};

// Index we expect to be on all shards, but with options ordered differently.
const indexWithOptionsOrderedDifferently = {
    d: 1
};

// Index we expect to be on all shards with the same name and key pattern, but with different
// options.
const indexWithDifferentOptions = {
    e: -1
};

// Filter expression to use over indexWithDifferentOptions.
const filterExpr = {
    d: {$gt: 50}
};

// Expiration time in seconds specified to expireAfterSeconds index option.
const expiration = 1000000;

// Create shared index on all shards.
assert.commandWorked(testDB[testName].createIndex(sharedIndex, {name: "sharedIndex"}));

// Create first missing index on every shard except the first.
assert.commandWorked(st.shard1.getDB(testDB)[testName].createIndex(
    indexMissingFromOneShard, {name: "indexMissingFromOneShard"}));
assert.commandWorked(st.shard2.getDB(testDB)[testName].createIndex(
    indexMissingFromOneShard, {name: "indexMissingFromOneShard"}));

// Create second missing index on only the first shard.
assert.commandWorked(st.shard0.getDB(testDB)[testName].createIndex(
    indexMissingFromTwoShards, {name: "indexMissingFromTwoShards", sparse: true}));

// Create index with same name and key pattern on all shards manually, but pass options ordered
// differently.
// In this case, we expect pipeline to recognize that these indexes are the same and NOT flag
// them as inconsistent.
assert.commandWorked(
    st.shard0.getDB(testDB)[testName].createIndex(indexWithOptionsOrderedDifferently, {
        name: "indexWithOptionsOrderedDifferently",
        partialFilterExpression: filterExpr,
        expireAfterSeconds: expiration
    }));
assert.commandWorked(
    st.shard1.getDB(testDB)[testName].createIndex(indexWithOptionsOrderedDifferently, {
        name: "indexWithOptionsOrderedDifferently",
        expireAfterSeconds: expiration,
        partialFilterExpression: filterExpr
    }));
assert.commandWorked(
    st.shard2.getDB(testDB)[testName].createIndex(indexWithOptionsOrderedDifferently, {
        partialFilterExpression: filterExpr,
        expireAfterSeconds: expiration,
        name: "indexWithOptionsOrderedDifferently"
    }));

// Create index with same name and key pattern on all shards manually, but with different options.
// In this case, we expect the pipeline to flag these as inconsistent.
assert.commandWorked(st.shard0.getDB(testDB)[testName].createIndex(indexWithDifferentOptions, {
    name: "indexWithDifferentOptions",
    partialFilterExpression: filterExpr,
    expireAfterSeconds: expiration
}));
assert.commandWorked(st.shard1.getDB(testDB)[testName].createIndex(
    indexWithDifferentOptions,
    {name: "indexWithDifferentOptions", expireAfterSeconds: expiration}));
assert.commandWorked(st.shard2.getDB(testDB)[testName].createIndex(
    indexWithDifferentOptions, {name: "indexWithDifferentOptions"}));

const result = testDB[testName].aggregate(pipeline).toArray();

// There are exactly 3 inconsistent indexes: two inconsistent across shards, and one with the
// same name, but different options.
let numInconsistentIndexes = 3;
assert.eq(result.length, numInconsistentIndexes);
for (const indexDoc of result) {
    assert.hasFields(indexDoc, ["indexName", "specs", "missingFromShards"]);
    const idxName = indexDoc["indexName"];
    const specList = indexDoc["specs"];
    const missingList = indexDoc["missingFromShards"];
    if (idxName === "indexWithDifferentOptions") {
        // All three shards have an index with the same name, but different options.
        assert.eq(specList.length, 3);
        // Not missing from any shard.
        assert.eq(missingList.length, 0);
        numInconsistentIndexes--;
    } else if (idxName === "indexMissingFromOneShard") {
        // Only missing from one shard: shard0
        assert.sameMembers(missingList, [st.shard0.shardName]);
        // Exactly one spec.
        assert.eq(specList.length, 1);
        assert.eq(specList[0]["key"], indexMissingFromOneShard);
        numInconsistentIndexes--;
    } else if (idxName === "indexMissingFromTwoShards") {
        // Missing from two shards: shard1 and shard2.
        assert.sameMembers(missingList, [st.shard1.shardName, st.shard2.shardName]);
        // Exactly one spec.
        assert.eq(specList.length, 1);
        assert.eq(specList[0]["key"], indexMissingFromTwoShards);
        numInconsistentIndexes--;
    }
}

// Verify that we've seen all 3 inconsistent indexes.
assert.eq(numInconsistentIndexes, 0);

st.stop();
})();