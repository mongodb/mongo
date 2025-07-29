/**
 * Provides utility functions for testing hybrid search queries on views.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {verifyExplainStagesAreEqual} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    assertDocArrExpectedFuzzy,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
assert.commandWorked(coll.createIndex({summary: "text", space: "text"}, {name: "textIndex"}));

const nDocs = 60;
let bulk = coll.initializeOrderedBulkOp();
// This test populates the collection with 5 different types of documents. This is to
// diversify/somewhat randomize the documents' contents such that it's easier to verify which
// documents satisfied the search criteria of the tests below.

// Populate the even-indexed documents
for (let i = 0; i < nDocs; i += 2) {
    // If the index is a multiple of 4 and 5 (ex: 0, 20 and 40), populate the document with m: 'baz'
    // , y: i / 2, and z: [2, 0, 1, 1, 4].
    if (i % 4 === 0 && i % 5 === 0) {
        bulk.insert({
            _id: i,
            a: "foo",
            m: "baz",
            x: i / 3,
            y: i / 2,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, 0, 1, 1, 4]
        });
    } else if (i % 4 === 0) {
        // If the index is only a multiple of 4 (ex: 4, 8, 12, 16, 24, 28, 32, 36, 44, 48), populate
        // the document with m: 'bar', y: i / 4, and z: [2, 0, 3, 1, 4].
        bulk.insert({
            _id: i,
            a: "foo",
            m: "bar",
            x: i / 3,
            y: i / 4,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, 0, 3, 1, 4]
        });
    } else {
        // If the index isn't a multiple of 4 (ex: 2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46),
        // populate the document with x: -1, y: 5, and v: [2, 0, 1, 1, 4]. Note that there are no
        // 'm'/'z' fields.
        bulk.insert({_id: i, a: "foo", x: -1, y: 5, loc: [i, i], v: [2, 0, 1, 1, 4]});
    }
}
// Populate the odd-indexed documents
for (let i = 1; i < nDocs; i += 2) {
    // If the index is a multiple of 3 (ex: 0, 3, 9, 15, 21, 27, 33, 39, 45), populate the document
    // with a: 'bar', y: i / 7, and z: [2, -2, 1, 4, 4]. Note that there is no 'm' field.
    if (i % 3 == 0) {
        bulk.insert({
            _id: i,
            a: "bar",
            x: i / 3,
            y: i / 7,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, -2, 1, 4, 4]
        });
    } else {
        // If the index isn't a multiple of 3 (ex: 1, 5, 7, 11, 13, 17, 19, 23, 25, 29, 31, 35, 37,
        // 41, 43, 47, 49), populate the document with a: 'bar', x: i / 2, y: i / 2, loc: [-i, -i],
        // v: [2, -2, 1, 4, 4], and  z: [1, 0, 8, 1, 8]. Note that there is no 'm' field.
        bulk.insert({
            _id: i,
            a: "bar",
            x: i / 2,
            y: i / 2,
            loc: [-i, -i],
            v: [2, -2, 1, 4, 4],
            z: [1, 0, 8, 1, 8]
        });
    }
}
assert.commandWorked(bulk.execute());

const searchIndex1Name = "searchIndex1";
const searchIndex2Name = "searchIndex2";
const vectorSearchIndex1Name = "vectorSearchIndex1";
const vectorSearchIndex2Name = "vectorSearchIndex2";

export const searchPipelineFoo = {
    $search: {index: searchIndex1Name, text: {query: "foo", path: "a"}}
};

export const searchPipelineBar = {
    $search: {index: searchIndex2Name, text: {query: "bar", path: "m"}}
};

export const vectorSearchPipelineV = {
    $vectorSearch: {
        queryVector: [1, 0, 8, 1, 8],
        path: "v",
        numCandidates: nDocs,
        index: vectorSearchIndex1Name,
        limit: nDocs,
    }
};

export const vectorSearchPipelineZ = {
    $vectorSearch: {
        queryVector: [2, 0, 3, 1, 4],
        path: "z",
        numCandidates: nDocs,
        index: vectorSearchIndex2Name,
        limit: nDocs,
    }
};

const mongotInputPipelines =
    new Set([searchPipelineFoo, searchPipelineBar, vectorSearchPipelineV, vectorSearchPipelineZ]);

export function createHybridSearchPipeline(
    inputPipelines, viewPipeline = null, stage, isRankFusion = true) {
    let hybridSearchStage = stage.$rankFusion;
    if (!isRankFusion) {
        hybridSearchStage = stage.$scoreFusion;
    }

    for (const [key, pipeline] of Object.entries(inputPipelines)) {
        if (viewPipeline) {
            hybridSearchStage.input.pipelines[key] = [...viewPipeline, ...pipeline];
            // A mongot stage must always be the first in the pipeline. Thus, the view pipeline
            // cannot be moved to the beginning. Placing the view pipeline and the rest of the
            // pipeline after this first stage achieves the same behavior.
            if (mongotInputPipelines.has(pipeline[0])) {
                hybridSearchStage.input.pipelines[key] =
                    [pipeline[0], ...viewPipeline, ...pipeline.splice(1)];
            }
        } else {
            // Otherwise, just use the input pipeline as is.
            hybridSearchStage.input.pipelines[key] = pipeline;
        }
    }

    return [stage];
}

const createSearchIndexes = (collOrView, indexNameSuffix = "") => {
    const searchIndexDef = {"mappings": {"dynamic": true}};
    createSearchIndex(collOrView,
                      {name: searchIndex1Name + indexNameSuffix, definition: searchIndexDef});
    createSearchIndex(collOrView,
                      {name: searchIndex2Name + indexNameSuffix, definition: searchIndexDef});
    const vectorSearchIndexDef = (path) => {
        return {
            "fields":
                [{"type": "vector", "numDimensions": 5, "path": path, "similarity": "euclidean"}]
        };
    };
    createSearchIndex(collOrView, {
        name: vectorSearchIndex1Name + indexNameSuffix,
        type: "vectorSearch",
        definition: vectorSearchIndexDef("v")
    });
    createSearchIndex(collOrView, {
        name: vectorSearchIndex2Name + indexNameSuffix,
        type: "vectorSearch",
        definition: vectorSearchIndexDef("z")
    });
};

const dropSearchIndexes = (collOrView, indexNameSuffix = "") => {
    dropSearchIndex(collOrView, {name: searchIndex1Name + indexNameSuffix});
    dropSearchIndex(collOrView, {name: searchIndex2Name + indexNameSuffix});
    dropSearchIndex(collOrView, {name: vectorSearchIndex1Name + indexNameSuffix});
    dropSearchIndex(collOrView, {name: vectorSearchIndex2Name + indexNameSuffix});
};

/**
 * This function runs a hybrid search ($rankFusion or $scoreFusion) aggregation query with and
 * without explain on the passed in collection (either the view's underlying collection or the view
 * itself).
 *
 * @param {DBCollection} collection the view's underlying collection or the view.
 * @param {object} pipeline will either be the pipeline with the view
 *     prepended or just the pipeline.
 */
function generateResults(collOrView, pipeline) {
    const results = collOrView.aggregate(pipeline);
    const explainResults = collOrView.explain().aggregate(pipeline);
    return [results, explainResults];
}

/**
 * This function runs aggregations against the view and its underlying collection with either search
 * indexes specified on both the view and underlying collection or just one of the two. The goal of
 * this test is to assert that the creation of indexes of different names doesn't impact the
 * aggregation results.
 *
 * @param {DBCollection} view the view on which to run aggregation queries.
 * @param {object} pipelineWithViewPrepended pipeline with the prepended view
 *     that is intended to be run on the view's underlying collection.
 * @param {object} pipelineWithoutView the assembled pipeline intended to be
 *     run on the view.
 * @param {boolean} checkCorrectness sometimes the correctness can differ for explain results, this
 *     flag gates whether the correctness for explain results should be checked.
 */
const runAggregationsWithDifferentSearchIndexCombinations =
    (view, pipelineWithViewPrepended, pipelineWithoutView, checkCorrectness) => {
        // Running a hybrid search query with a $search input pipeline that specifies a search index
        // on the view's underlying collection when the view does not have a search index is
        // allowed.
        createSearchIndexes(coll);
        const [expectedResultsNoSearchIndexOnView, expectedExplainNoSearchIndexOnView] =
            generateResults(coll, pipelineWithViewPrepended);

        // Running a hybrid search query with a $search input pipeline that specifies a search index
        // on the view's underlying collection when the view ALSO has a search index of a different
        // name is allowed.
        createSearchIndexes(view, "_view");
        const [expectedResultsWithSearchIndexOnView, expectedExplainWithSearchIndexOnView] =
            generateResults(coll, pipelineWithViewPrepended);

        // The expected results and expected explain results should be the same since the above
        // aggregation queries only specify indexes on the underlying collection. Thus, the
        // creation of indexes on the view should not impact the results.
        assert.eq(expectedResultsNoSearchIndexOnView, expectedResultsWithSearchIndexOnView);
        assert.neq(expectedExplainNoSearchIndexOnView["_batch"], []);
        assert.neq(expectedExplainWithSearchIndexOnView["_batch"], []);

        // Drop all the search indexes indexes with the same name as those on the underlying
        // collection can be created on the view instead.
        dropSearchIndexes(coll);
        dropSearchIndexes(view, "_view");

        // Running a hybrid search query with a $search input pipeline that specifies a search index
        // on the view when the view's underlying collection does not have a search index is
        // allowed.
        createSearchIndexes(view);
        const [viewResultsNoSearchIndexOnColl, viewExplainNoSearchIndexOnColl] =
            generateResults(view, pipelineWithoutView);

        // Running a hybrid search query with a $search input pipeline that specifies a search index
        // on the view when the view's underlying collection has a search index of a different name
        // is allowed.
        createSearchIndexes(coll, "_coll");
        const [viewResultsWithSearchIndexOnColl, viewExplainWithSearchIndexOnColl] =
            generateResults(view, pipelineWithoutView);

        // The expected results and expected explain results should be the same since the above
        // aggregation queries only specify indexes on the view. Thus, the creation of indexes on
        // the underlying collection should not impact the results.
        assert.eq(viewResultsNoSearchIndexOnColl, viewResultsWithSearchIndexOnColl);
        assert.neq(viewExplainNoSearchIndexOnColl["_batch"], []);
        assert.neq(viewExplainWithSearchIndexOnColl["_batch"], []);

        // Verify that the results from running the queries on the underylying collection and view
        // match.
        if (checkCorrectness) {
            assertDocArrExpectedFuzzy(expectedResultsNoSearchIndexOnView.toArray(),
                                      viewResultsNoSearchIndexOnColl.toArray());

            assertDocArrExpectedFuzzy(expectedResultsWithSearchIndexOnView.toArray(),
                                      viewResultsWithSearchIndexOnColl.toArray());
        }
        // Drop all the search indexes to avoid collisions, creating indexes of the same name, in
        // future invocations of this method.
        dropSearchIndexes(view);
        dropSearchIndexes(coll, "_coll");
    };

/**
 * This function creates a view with the provided name and pipeline, runs a hybrid search query
 * against the view, then runs the same query against the main collection with the view stage
 * prepended to the input pipelines in the same way that the view desugaring works. It compares both
 * the results and the explain output of both queries to ensure they are the same.
 *
 * @param {string} testName name of the test, used to create a unique view.
 * @param {object} inputPipelines spec for hybrid search stage's input pipelines; can be as many as
 *     needed.
 * @param {array}  viewPipeline pipeline to create a view and to be prepended manually to the input
 *                              pipelines.
 * @param {bool}   checkCorrectness some pipelines are able to parse and run, but the order of the
 *                                  stages or the stages themselves are non-deterministic, so we
 *                                  can't check for correctness.
 * @param {bool}   isMongotPipeline true if the input pipelines are mongot pipelines (contains a
 *     search or vectorSearch stage). Use this to determine whether to compare explain results since
 *     they will otherwise differ if the hybrid search stage has a hybrid search input pipeline.
 */
export const runHybridSearchViewTest = (testName,
                                        inputPipelines,
                                        viewPipeline,
                                        checkCorrectness,
                                        isMongotPipeline = false,
                                        createStagePipelineFn) => {
    // Create a view with viewStage.
    const viewName = jsTestName() + "_" + testName + "_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    // Create the pipeline with the view stage manually prepended.
    const pipelineWithViewPrepended = createStagePipelineFn(inputPipelines, viewPipeline);

    // Create the pipeline without the view stage
    const pipelineWithoutView = createStagePipelineFn(inputPipelines);

    if (isMongotPipeline) {
        runAggregationsWithDifferentSearchIndexCombinations(
            view, pipelineWithViewPrepended, pipelineWithoutView, checkCorrectness);
    } else {
        // Running a hybrid search query over the main collection with the view stage prepended
        // succeeds.
        const [expectedResultsNoSearchIndexOnView, expectedExplainNoSearchIndexOnView] =
            generateResults(coll, pipelineWithViewPrepended);

        // Running a hybrid search query against the view succeeds too.
        const [viewResultsNoSearchIndexOnColl, viewExplainNoSearchIndexOnColl] =
            generateResults(view, pipelineWithoutView);

        // Verify the explain stages match.
        verifyExplainStagesAreEqual(viewExplainNoSearchIndexOnColl,
                                    expectedExplainNoSearchIndexOnView);

        // Verify the results match.
        if (checkCorrectness) {
            assertDocArrExpectedFuzzy(expectedResultsNoSearchIndexOnView.toArray(),
                                      viewResultsNoSearchIndexOnColl.toArray());
        }
    }
};

// Test a $unionWith following a hybrid search stage ($rankFusion or $scoreFusion) to verify that
// the stage's desugaring doesn't interfere with view resolution of the user provided $unionWith.
export function testHybridSearchViewWithSubsequentUnionOnSameView(inputPipelines,
                                                                  createStagePipelineFn) {
    const viewPipeline = [{$sort: {x: -1}}, {$limit: 5}];

    // Create a view with viewStage.
    const viewName = jsTestName() + "_subsequent_union_with_on_same_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    // Create the pipeline with the view stage manually prepended.
    const hybridSearchPipelineWithViewPrepended =
        createStagePipelineFn(inputPipelines, viewPipeline);

    // Create the pipeline without the view stage
    const hybridSearchPipelineWithoutView = createStagePipelineFn(inputPipelines);

    // Create the subsequent $unionWith, on the same view as the query.
    const unionWithSpec = {
        $unionWith: {coll: viewName, pipeline: [{$sort: {_id: -1}}, {$limit: 2}]}
    };

    // Run the two queries. They should return 5 documents - the hybrid search stage + the filter
    // pipeline will together return the 3 documents with the lowest ids, and the $unionWith should
    // return the 2 documents with the highest ids. The values were manually checked from logs. Note
    // that these queries do not make sense in the real world, but serve to test that the $unionWith
    // runs on the correct set of documents w/ the view pipeline.

    // This query will contain a pipeline of [hybrid search stage, $unionWith] where the hybrid
    // search stage runs against the base collection and $unionWith runs against a view.
    const filterPipeline = [{$sort: {_id: 1}}, {$limit: 3}];

    const pipelineWithViewPrepended =
        [...hybridSearchPipelineWithViewPrepended, ...filterPipeline, ...[unionWithSpec]];
    let resultsFromViewPrependedHybridSearchQuery =
        coll.aggregate(pipelineWithViewPrepended).toArray();

    // This query will contain a pipeline of [hybrid search stage, $unionWith] where the hybrid
    // search stage AND the $unionWith are run against the same view.
    const pipelineWithoutViewPrepended =
        [...hybridSearchPipelineWithoutView, ...filterPipeline, ...[unionWithSpec]];
    let resultsFromViewlessHybridSearchQuery =
        view.aggregate(pipelineWithoutViewPrepended).toArray();

    // Verify the results match.
    assert.eq(resultsFromViewPrependedHybridSearchQuery.length,
              resultsFromViewlessHybridSearchQuery.length);
    assert.eq(resultsFromViewPrependedHybridSearchQuery.length, 5);
    assertDocArrExpectedFuzzy(resultsFromViewPrependedHybridSearchQuery,
                              resultsFromViewlessHybridSearchQuery);
}

// Test a $unionWith following a hybrid search stage ($rankFusion or $scoreFusion) to verify that
// the stage's desugaring doesn't interfere with view resolution of the user provided $unionWith on
// a different view.
export function testHybridSearchViewWithSubsequentUnionOnDifferentView(inputPipelines,
                                                                       createStagePipelineFn) {
    const viewPipeline = [{$sort: {x: -1}}, {$limit: 5}];

    // Create a view with viewStage.
    const viewName = jsTestName() + "_subsequent_union_with_on_different_view";
    assert.commandWorked(db.createView(viewName, coll.getName(), viewPipeline));
    const view = db[viewName];

    // Create the hybrid search pipeline with the view stage manually prepended.
    const hybridSearchPipelineWithViewPrepended =
        createStagePipelineFn(inputPipelines, viewPipeline);

    // Create the hybrid search pipeline without the view stage
    const hybridSearchPipelineWithoutView = createStagePipelineFn(inputPipelines);

    // Create the subsequent $unionWith, on a different view as the query.
    const unionWithViewPipeline = [{$sort: {_id: 1, x: 1}}, {$limit: 5}];
    const unionWithViewName =
        jsTestName() + "_subsequent_union_with_on_different_view_union_with_view";
    assert.commandWorked(db.createView(unionWithViewName, coll.getName(), unionWithViewPipeline));
    const unionWithSpec = {
        $unionWith: {coll: unionWithViewName, pipeline: [{$match: {x: {$lt: 15}}}]}
    };

    // Run the two queries. They should return the 5 documents with the highest $x values (from the
    // hybrid search stage) and 5 documents with $x values < 15. The values were manually checked
    // from logs. Note that these queries do not make sense in the real world, but serve to test
    // that the $unionWith runs on the correct set of documents w/ the view pipeline.

    // This query will contain a pipeline of [hybrid search stage, $unionWith] where the hybrid
    // search stage runs against the base collection and $unionWith runs against its own view.
    const pipelineWithViewPrepended =
        [...hybridSearchPipelineWithViewPrepended, ...[unionWithSpec]];
    let resultsFromViewPrependedHybridSearchQuery =
        coll.aggregate(pipelineWithViewPrepended).toArray();

    // This query will contain a pipeline of [hybrid search stage, $unionWith] where the hybrid
    // search stage runs against one view and the $unionWith runs against another view.
    const pipelineWithoutViewPrepended = [...hybridSearchPipelineWithoutView, ...[unionWithSpec]];
    let resultsFromViewlessHybridSearchQuery =
        view.aggregate(pipelineWithoutViewPrepended).toArray();

    // Verify the results match.
    assert.eq(resultsFromViewPrependedHybridSearchQuery.length,
              resultsFromViewlessHybridSearchQuery.length);
    assert.eq(resultsFromViewPrependedHybridSearchQuery.length, 10);
    assertDocArrExpectedFuzzy(resultsFromViewPrependedHybridSearchQuery,
                              resultsFromViewlessHybridSearchQuery);
}
