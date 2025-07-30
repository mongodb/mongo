/**
 * Tests that having a view defined with $search/$vectorSearch works correctly at the top level
 * command, inside a $unionWith, and inside a $lookup.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_82 ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    getMovieData,
    getMoviePlotEmbeddingById,
    getMovieSearchIndexSpec,
    getMovieVectorSearchIndexSpec,
} from "jstests/with_mongot/e2e_lib/data/movies.js";
import {
    assertDocArrExpectedFuzzy,
    buildExpectedResults,
    datasets,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany(getMovieData()));

// These are the pipelines we will test in the views and queries ran against said views.
const matchPipeline = [{$match: {$expr: {$in: ["Adventure", "$genres"]}}}];
const buildSearchPipeline = (indexName) => [{
    $search:
        {index: indexName, text: {query: "ape", path: ["fullplot", "title"]}, scoreDetails: true}
}];
const buildVectorSearchPipeline = (indexName) => [{
    $vectorSearch: {
        // Get the embedding for 'Tarzan the Ape Man', which has _id = 6.
        queryVector: getMoviePlotEmbeddingById(6),
        path: "plot_embedding",
        numCandidates: 200,
        index: indexName,
        limit: 20,
    }
}];
// The $match will not match any documents, so everything that the pipeline returns is the documents
// from the $unionWith, making it the same as running the subPipeline against the namespace at the
// top level.
const buildUnionWithPipeline =
    (namespace, subPipeline) => [{$match: {title: "Unexistent Movie"}},
                                 {$unionWith: {coll: namespace, pipeline: subPipeline}},
];
// The $limit + $unwind + $replaceRoot combo will make this pipeline the same as running
// subPipeline against the namespace at the top level.
const buildLookupPipeline =
    (namespace,
     subPipeline) => [{
        $limit: 1,
    },
                      {$lookup: {from: namespace, as: "matched_docs", pipeline: subPipeline}},
                      {$unwind: "$matched_docs"},
                      {$replaceRoot: {newRoot: "$matched_docs"}}];

// We will try different combinations of views and pipelines.
const searchViewName = jsTestName() + "_search_view";
const vectorSearchViewName = jsTestName() + "_vector_search_view";
const matchViewName = jsTestName() + "_match_view";
const unionWithViewName = jsTestName() + "_union_with_view";
const lookupViewName = jsTestName() + "_lookup_view";

// We need an index on the collection so that the views containing $search can work, and the one on
// the match view so that the queries with $search against said view can work as well.
const searchIndexOnCollName = 'search_movie_coll';
const searchIndexOnMatchViewName = 'search_movie_match_view';
const vectorSearchIndexOnCollName = 'vector_search_movie_coll';
const vectorSearchIndexOnMatchViewName = 'vector_search_match_view';

// Create the views.
assert.commandWorked(
    db.createView(searchViewName, collName, buildSearchPipeline(searchIndexOnCollName)));
assert.commandWorked(db.createView(
    vectorSearchViewName, collName, buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
assert.commandWorked(
    db.createView(unionWithViewName,
                  collName,
                  buildUnionWithPipeline(collName, buildSearchPipeline(searchIndexOnCollName))));
assert.commandWorked(
    db.createView(lookupViewName,
                  collName,
                  buildLookupPipeline(collName, buildSearchPipeline(searchIndexOnCollName))));
assert.commandWorked(db.createView(matchViewName, collName, matchPipeline));

const searchView = db[searchViewName];
const vectorSearchView = db[vectorSearchViewName];
const matchView = db[matchViewName];
const unionWithView = db[unionWithViewName];
const lookupView = db[lookupViewName];

// Create the mongot indexes on the collection.
createSearchIndex(coll,
                  {name: searchIndexOnCollName, definition: getMovieSearchIndexSpec().definition});
createSearchIndex(coll, {
    name: vectorSearchIndexOnCollName,
    type: "vectorSearch",
    definition: getMovieVectorSearchIndexSpec().definition
});

// Create the mongot indexes on the $match view.
createSearchIndex(
    matchView,
    {name: searchIndexOnMatchViewName, definition: getMovieSearchIndexSpec().definition});
createSearchIndex(matchView, {
    name: vectorSearchIndexOnMatchViewName,
    type: "vectorSearch",
    definition: getMovieVectorSearchIndexSpec().definition
});

// There are only 4 sets of expected results:
// 1. The $search query running fully.
// 2. The $vectorSearch query running fully.
// 3. The $search query + the $match filter.
// 4. The $vectorSearch query + the $match filter.
//
// The $unionWith and $lookup pipelines are just wrappers around the sub-pipelines, so they
// will return the same results as running the sub-pipelines at the top level.
const expectedResultsSearch = buildExpectedResults([6, 1, 2, 3, 4, 5], datasets.MOVIES);
const expectedResultsVectorSearch =
    buildExpectedResults([6, 4, 8, 9, 10, 12, 13, 5, 1, 14, 3, 2, 11, 7, 15], datasets.MOVIES);
const expectedResultsMatchSearch = buildExpectedResults([6, 2, 4, 5], datasets.MOVIES);
const expectedResultsMatchVectorSearch =
    buildExpectedResults([6, 4, 8, 9, 12, 13, 5, 2, 11], datasets.MOVIES);

const runTest = (collOrView, pipeline, expectedResults) => {
    const results = collOrView.aggregate(pipeline).toArray();
    assertDocArrExpectedFuzzy(expectedResults, results);

    // Confirm that running explain works.
    assert.commandWorked(
        collOrView.runCommand("aggregate", {pipeline: pipeline, explain: true, cursor: {}}));
};

const runTestFails = (collOrView, pipeline, isShardedLookup = false) => {
    assert.commandFailedWithCode(
        collOrView.runCommand("aggregate", {pipeline: pipeline, explain: false, cursor: {}}),
        [10623000, 10623001]);

    // Sharded lookups will successfully run explain, all other topologies will fail on explain.
    if (isShardedLookup) {
        assert.commandWorked(
            collOrView.runCommand("aggregate", {pipeline: pipeline, explain: true, cursor: {}}));
    } else {
        assert.commandFailedWithCode(
            collOrView.runCommand("aggregate", {pipeline: pipeline, explain: true, cursor: {}}),
            [10623000, 10623001]);
    }
};

const isShardedCollection = coll.stats().sharded;

// ==================================================================
// $search tests
// ==================================================================
(function matchAgainstSearchView() {
    runTest(searchView, matchPipeline, expectedResultsMatchSearch);
})();

(function searchAgainstSearchView() {
    runTestFails(searchView, buildSearchPipeline(searchIndexOnCollName));
})();

(function matchSubpipelineFromSearchViewAgainstTopLevelSearchView() {
    runTest(searchView,
            buildUnionWithPipeline(searchViewName, matchPipeline),
            expectedResultsMatchSearch);
    runTest(
        searchView, buildLookupPipeline(searchViewName, matchPipeline), expectedResultsMatchSearch);
})();

(function lookupSearchSubpipelineFromSearchView() {
    runTestFails(coll,
                 buildLookupPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(matchView,
                 buildLookupPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(searchView,
                 buildLookupPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(unionWithView,
                 buildLookupPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(lookupView,
                 buildLookupPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
})();

(function unionWithSearchSubpipelineFromSearchView() {
    runTestFails(
        coll, buildUnionWithPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(
        matchView,
        buildUnionWithPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(
        searchView,
        buildUnionWithPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(
        unionWithView,
        buildUnionWithPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(
        lookupView,
        buildUnionWithPipeline(searchViewName, buildSearchPipeline(searchIndexOnCollName)));
})();

(function searchSubpipelineFromCollection() {
    runTest(searchView,
            buildUnionWithPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
    runTest(searchView,
            buildLookupPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
    runTest(unionWithView,
            buildUnionWithPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
    runTest(unionWithView,
            buildLookupPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
    runTest(lookupView,
            buildUnionWithPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
    runTest(lookupView,
            buildLookupPipeline(collName, buildSearchPipeline(searchIndexOnCollName)),
            expectedResultsSearch);
})();

(function searchSubpipelineFromMatchView() {
    runTest(searchView,
            buildUnionWithPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
    runTest(searchView,
            buildLookupPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
    runTest(unionWithView,
            buildUnionWithPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
    runTest(unionWithView,
            buildLookupPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
    runTest(lookupView,
            buildUnionWithPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
    runTest(lookupView,
            buildLookupPipeline(matchViewName, buildSearchPipeline(searchIndexOnMatchViewName)),
            expectedResultsMatchSearch);
})();

(function searchSubpipelineFromUnionWithView() {
    runTestFails(
        searchView,
        buildUnionWithPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(searchView,
                 buildLookupPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        unionWithView,
        buildUnionWithPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(unionWithView,
                 buildLookupPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        lookupView,
        buildUnionWithPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(lookupView,
                 buildLookupPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        matchView,
        buildUnionWithPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(matchView,
                 buildLookupPipeline(unionWithViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
})();

(function searchSubpipelineFromLookupView() {
    runTestFails(
        searchView,
        buildUnionWithPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(searchView,
                 buildLookupPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        unionWithView,
        buildUnionWithPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(unionWithView,
                 buildLookupPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        lookupView,
        buildUnionWithPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(lookupView,
                 buildLookupPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
    runTestFails(
        matchView,
        buildUnionWithPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)));
    runTestFails(matchView,
                 buildLookupPipeline(lookupViewName, buildSearchPipeline(searchIndexOnCollName)),
                 isShardedCollection);
})();

// ==================================================================
// $vectorSearch tests
// ==================================================================
(function matchAgainstVectorSearchView() {
    runTest(vectorSearchView, matchPipeline, expectedResultsMatchVectorSearch);
})();

(function vectorSearchAgainstVectorSearchView() {
    runTestFails(vectorSearchView, buildVectorSearchPipeline(vectorSearchIndexOnCollName));
})();

(function matchSubpipelineFromVectorSearchViewAgainstTopLevelVectorSearchView() {
    runTest(vectorSearchView,
            buildUnionWithPipeline(vectorSearchViewName, matchPipeline),
            expectedResultsMatchVectorSearch);
})();

(function unionWithVectorSearchSubpipelineFromVectorSearchView() {
    runTestFails(coll,
                 buildUnionWithPipeline(vectorSearchViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(matchView,
                 buildUnionWithPipeline(vectorSearchViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(vectorSearchView,
                 buildUnionWithPipeline(vectorSearchViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(unionWithView,
                 buildUnionWithPipeline(vectorSearchViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(lookupView,
                 buildUnionWithPipeline(vectorSearchViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
})();

(function vectorSearchSubpipelineFromCollection() {
    runTest(
        vectorSearchView,
        buildUnionWithPipeline(collName, buildVectorSearchPipeline(vectorSearchIndexOnCollName)),
        expectedResultsVectorSearch);
    runTest(
        unionWithView,
        buildUnionWithPipeline(collName, buildVectorSearchPipeline(vectorSearchIndexOnCollName)),
        expectedResultsVectorSearch);
    runTest(
        lookupView,
        buildUnionWithPipeline(collName, buildVectorSearchPipeline(vectorSearchIndexOnCollName)),
        expectedResultsVectorSearch);
})();

(function vectorSearchSubpipelineFromMatchView() {
    runTest(vectorSearchView,
            buildUnionWithPipeline(matchViewName,
                                   buildVectorSearchPipeline(vectorSearchIndexOnMatchViewName)),
            expectedResultsMatchVectorSearch);
    runTest(unionWithView,
            buildUnionWithPipeline(matchViewName,
                                   buildVectorSearchPipeline(vectorSearchIndexOnMatchViewName)),
            expectedResultsMatchVectorSearch);
    runTest(lookupView,
            buildUnionWithPipeline(matchViewName,
                                   buildVectorSearchPipeline(vectorSearchIndexOnMatchViewName)),
            expectedResultsMatchVectorSearch);
})();

(function vectorSearchSubpipelineFromUnionWithView() {
    runTestFails(vectorSearchView,
                 buildUnionWithPipeline(unionWithViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(unionWithView,
                 buildUnionWithPipeline(unionWithViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(lookupView,
                 buildUnionWithPipeline(unionWithViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(matchView,
                 buildUnionWithPipeline(unionWithViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
})();

(function vectorSearchSubpipelineFromLookupView() {
    runTestFails(vectorSearchView,
                 buildUnionWithPipeline(lookupViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(unionWithView,
                 buildUnionWithPipeline(lookupViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(lookupView,
                 buildUnionWithPipeline(lookupViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
    runTestFails(matchView,
                 buildUnionWithPipeline(lookupViewName,
                                        buildVectorSearchPipeline(vectorSearchIndexOnCollName)));
})();

dropSearchIndex(coll, {name: searchIndexOnCollName});
dropSearchIndex(matchView, {name: searchIndexOnMatchViewName});
dropSearchIndex(coll, {name: vectorSearchIndexOnCollName});
dropSearchIndex(matchView, {name: vectorSearchIndexOnMatchViewName});
