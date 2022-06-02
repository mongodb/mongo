// Test that the $facet stage reports the correct stats in serverStatus().metrics.queryExecutor.
// @tags: [
//  # Should not run on sharded suites due to use of serverStatus()
//  assumes_unsharded_collection,
//  assumes_no_implicit_collection_creation_after_drop,
//  do_not_wrap_aggregations_in_facets,
//  assumes_read_preference_unchanged,
//  assumes_read_concern_unchanged,
//  assumes_against_mongod_not_mongos,
//  does_not_support_repeated_reads,
// ]

(function() {
"use strict";

const testDB = db.getSiblingDB("facet_stats");
const local = testDB.facetLookupLocal;
const foreign = testDB.facetLookupForeign;
testDB.dropDatabase();

let runFacetPipeline = function() {
    const lookup = {
        $lookup: {
            from: foreign.getName(),
            let: {id1: "$_id"},
            pipeline: [{$match: {$expr: {$eq: ["$$id1", "$foreignKey"]}}}],
            as: "joined"
        }
    };

    return local.aggregate([{$facet: {nested: [lookup]}}]).itcount();
};

assert.commandWorked(local.insert({_id: 1, score: 100}));
assert.commandWorked(local.insert({_id: 2, score: 200}));
assert.commandWorked(local.insert({_id: 3, score: 200}));

assert.commandWorked(foreign.insert({_id: 0, foreignKey: 1}));
assert.commandWorked(foreign.insert({_id: 1, foreignKey: 2}));
assert.commandWorked(foreign.insert({_id: 2, foreignKey: 3}));

let queryExecutor = testDB.serverStatus().metrics.queryExecutor;
let curScannedObjects = queryExecutor.scannedObjects;
let curScannedKeys = queryExecutor.scanned;

assert.eq(1, runFacetPipeline());

queryExecutor = testDB.serverStatus().metrics.queryExecutor;
curScannedObjects = queryExecutor.scannedObjects - curScannedObjects;
curScannedKeys = queryExecutor.scanned - curScannedKeys;
// For each document on the local side, this query has to perform a scan of the foreign side.
// Therefore, the total number of documents examined is
// cardinality(local) + cardinality(local) * cardinality(foreign) = 3 + 3 * 3 = 12.
assert.eq(12, curScannedObjects);
// $facet sub-pipelines cannot make use of indexes. Hence scanned keys should be 0.
assert.eq(0, curScannedKeys);
})();
