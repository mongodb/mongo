/**
 * Test an aggregation pipeline optimization where a $sort stage can be removed after
 * a $vectorSearch stage, given that the $sort is on the same criteria that the $vectorSearch
 * results are sorted by (the 'vectorSearchScore') .
 *
 * Also, this test should only run in single-node environments because a $sort after a $vectorSearch
 * in a sharded cluster will end up with the $vectorSearch on mongod and $sort on mongos.
 *
 * @tags: [featureFlagRankFusionFull, requires_fcv_81, assumes_against_mongod_not_mongos]
 */

import {getIndexOfStageOnSingleNode} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

// Helper functions to check if optimization being tested for exists:

function assertSortExistsAfterVectorSearch(aggPipeline) {
    let explain = coll.explain().aggregate(aggPipeline);
    // $vectorSearch must be the first step of the pipeline
    assert(
        getIndexOfStageOnSingleNode(explain, "$vectorSearch") == 0,
        "'$vectorSearch' is not first step of the pipeline. explain for query: " + tojson(explain));
    // A $sort stage must exist somewhere in the pipeline after $_internalSearchMongotRemote.
    assert(getIndexOfStageOnSingleNode(explain, "$sort") > 0,
           "'$sort' does not exist in the pipeline after $search. explain for query: " +
               tojson(explain));
}

function assertNoSortExistsAfterVectorSearch(aggPipeline) {
    let explain = coll.explain().aggregate(aggPipeline);
    // $vectorSearch must be the first step of the pipeline
    assert(
        getIndexOfStageOnSingleNode(explain, "$vectorSearch") == 0,
        "'$vectorSearch' is not first step of the pipeline. explain for query: " + tojson(explain));
    // A $sort stage must not exist somewhere in the pipeline after $_internalSearchMongotRemote.
    assert(
        getIndexOfStageOnSingleNode(explain, "$sort") < 0,
        "'$sort' does exist in the pipeline after $search. explain for query: " + tojson(explain));
}

const coll = db.foo;
coll.drop();

assert.commandWorked(coll.insertMany(
    [{a: -1, v: [1, 0, 8, 1, 8]}, {a: 100, v: [2, -2, 1, 4, 4]}, {a: 10, v: [4, 10, -8, 22, 0]}]));

const indexName = "sort-after-vector-search-test-index";
// Create vector search index on movie plot embeddings.
const vectorIndex = {
    name: indexName,
    type: "vectorSearch",
    definition:
        {"fields": [{"type": "vector", "numDimensions": 5, "path": "v", "similarity": "euclidean"}]}
};

createSearchIndex(coll, vectorIndex);

const vectorSearchQuery = {
    queryVector: [2, 4, -8, 2, 10],
    path: "v",
    numCandidates: 3,
    index: indexName,
    limit: 3,
};

// Run test cases:
//
// Cases where optimization applies and $sort should be removed:

// Standard case where a single sort on 'vectorSearchScore' should be removed.
assertNoSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
]);

// Multiple sorts in a row should all be removed.
assertNoSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
    {$limit: 10},
]);

// Implicit $sort after $vectorSearch from desugared $setWindowFields should get removed.
assertNoSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$setWindowFields: {sortBy: {score: {$meta: "vectorSearchScore"}}, output: {rank: {$rank: {}}}}}
]);

// Mixed explicit and implicit $sort after $vectorSearch should both get removed.
assertNoSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
    {$setWindowFields: {sortBy: {score: {$meta: "vectorSearchScore"}}, output: {rank: {$rank: {}}}}}
]);

// Cases where optimization should not apply and $sort should remain:

// Explicit $sort that does not sort on 'vectorSearchScore' should not be removed.
assertSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$sort: {a: 1}},
]);

// $sort with multi-field criteria on 'vectorSearchScore' and another field should not be removed.
assertSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$sort: {score: {$meta: "vectorSearchScore"}, a: 1}},
]);

// Currently cannot optimize $sort that is not directly after $vectorSearch.
// TODO SERVER-96068: check that $sort is removed for these types of pipelines.
assertSortExistsAfterVectorSearch([
    {$vectorSearch: vectorSearchQuery},
    {$limit: 10},
    {$sort: {score: {$meta: "vectorSearchScore"}}},
]);

dropSearchIndex(coll, {name: indexName});
