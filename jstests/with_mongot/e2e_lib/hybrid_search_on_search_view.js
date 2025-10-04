/**
 * Provides utilities to test that hybrid search stages on a view namespace, defined with a
 * search pipeline, is allowed and works correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_82]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertDocArrExpectedFuzzy} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

const nDocs = 50;
let bulk = coll.initializeOrderedBulkOp();
// This test populates the collection with 2 different types of documents. This is to
// diversify/somewhat randomize the documents' contents such that it's easier to verify which
// documents satisfied the search criteria of the tests below.

// Populate the even-indexed documents
for (let i = 0; i < nDocs; i += 2) {
    // If the index is a multiple of 4 and 5 (ex: 0, 20 and 40), populate the document with m: 'baz'
    // y: i - 100, and z: [2, 0, 1, 1, 4].
    if (i % 4 === 0 && i % 5 === 0) {
        bulk.insert({
            _id: i,
            a: "foo",
            m: "baz",
            x: i / 3,
            y: i - 100,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, 0, 1, 1, 4],
        });
    } else if (i % 4 === 0) {
        // If the index is only a multiple of 4 (ex: 4, 8, 12, 16, 24, 28, 32, 36, 44, 48), populate
        // the document with m: 'bar', y: i + 100, and z: [2, 0, 3, 1, 4].
        bulk.insert({
            _id: i,
            a: "foo",
            m: "bar",
            x: i / 3,
            y: i + 100,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, 0, 3, 1, 4],
        });
    } else {
        // If the index isn't a multiple of 4 (ex: 2, 6, 10, 14, 18, 22, 26, 30, 34, 38, 42, 46),
        // populate the document with x: -1, y: i / 2, and v: [2, 0, 1, 1, 4]. Note that there are
        // no 'm'/'z' fields.
        bulk.insert({_id: i, a: "foo", x: -1, y: i / 2, loc: [i, i], v: [2, 0, 1, 1, 4]});
    }
}
// Populate the odd-indexed documents
for (let i = 1; i < nDocs; i += 2) {
    // If the index is a multiple of 3 (ex: 0, 3, 9, 15, 21, 27, 33, 39, 45), populate the document
    // with a: 'bar', y: i + 100, and z: [2, -2, 1, 4, 4]. Note that there is no 'm' field.
    if (i % 3 == 0) {
        bulk.insert({
            _id: i,
            a: "bar",
            x: i / 3,
            y: i + 100,
            loc: [i, i],
            v: [1, 0, 8, 1, 8],
            z: [2, -2, 1, 4, 4],
        });
    } else {
        // If the index isn't a multiple of 3 (ex: 1, 5, 7, 11, 13, 17, 19, 23, 25, 29, 31, 35, 37,
        // 41, 43, 47, 49), populate the document with a: 'bar', x: i / 2, loc: [-i, -i], v: [2, -2,
        // 1, 4, 4], y: i - 100, and  z: [1, 0, 8, 1, 8]. Note that there is no 'm' field.
        bulk.insert({
            _id: i,
            a: "bar",
            x: i / 2,
            y: i - 100,
            loc: [-i, -i],
            v: [2, -2, 1, 4, 4],
            z: [1, 0, 8, 1, 8],
        });
    }
}
assert.commandWorked(bulk.execute());

const searchIndexFoo = "searchIndexFoo";
const vectorSearchIndexV = "vectorSearchIndexV";

createSearchIndex(coll, {name: searchIndexFoo, definition: {"mappings": {"dynamic": true}}});

createSearchIndex(coll, {
    name: vectorSearchIndexV,
    type: "vectorSearch",
    definition: {"fields": [{"type": "vector", "numDimensions": 5, "path": "v", "similarity": "euclidean"}]},
});

export const searchPipelineFoo = {
    $search: {index: searchIndexFoo, text: {query: "foo", path: "a"}},
};

export const searchPipelineBar = {
    $search: {index: "searchIndexBar", text: {query: "bar", path: "m"}},
};

export const vectorSearchPipelineV = {
    $vectorSearch: {
        queryVector: [1, 0, 8, 1, 8],
        path: "v",
        numCandidates: nDocs,
        index: vectorSearchIndexV,
        limit: nDocs,
    },
};

export const vectorSearchPipelineZ = {
    $vectorSearch: {
        queryVector: [2, 0, 3, 1, 4],
        path: "z",
        numCandidates: nDocs,
        index: "vectorSearchIndexZ",
        limit: nDocs,
    },
};

const searchPipelineFooNoIndex = {
    $search: {text: {query: "foo", path: "a"}},
};

const searchIndexDef = {
    name: jsTestName() + "_index",
    definition: {mappings: {dynamic: true}},
};

const mongotInputPipelines = new Set([
    searchPipelineFoo,
    searchPipelineBar,
    vectorSearchPipelineV,
    vectorSearchPipelineZ,
]);

export function createHybridSearchPipeline(inputPipelines, viewPipeline, stage, isRankFusion = true) {
    let hybridSearchStage = stage.$rankFusion;
    if (!isRankFusion) {
        hybridSearchStage = stage.$scoreFusion;
    }

    for (const [key, pipeline] of Object.entries(inputPipelines)) {
        if (viewPipeline) {
            // If the input pipelines contain a mix of mongot and non-mongot input pipelines,
            // construct a hybrid search stage stage that only contains the non-mongot input
            // pipelines.
            if (!mongotInputPipelines.has(pipeline[0])) {
                hybridSearchStage.input.pipelines[key] = [...viewPipeline, ...pipeline];
            }
        } else {
            // Otherwise, just use the input pipeline as is.
            hybridSearchStage.input.pipelines[key] = [...pipeline];
        }
    }

    return [stage];
}

// Note that the first pipeline doesn't specify a searchIndex while the second one does.
const viewPipelines = [[searchPipelineFooNoIndex], [searchPipelineFoo], [vectorSearchPipelineV]];

const createViews = () => {
    const viewPipelineNames = ["searchNoIndex", "searchWithIndex", "vectorSearch"];
    const views = [];
    const viewNames = [];
    for (let i = 0; i < viewPipelines.length; i++) {
        const viewName = jsTestName() + "_" + viewPipelineNames[i] + "_view";
        assert.commandWorked(db.createView(viewName, coll.getName(), viewPipelines[i]));
        const searchView = db[viewName];
        viewNames.push(viewName);
        views.push(searchView);
    }
    return [viewNames, views];
};

const [viewNames, views] = createViews();

export function runHybridSearchOnSearchViewsTest(inputPipelines, checkCorrectness = true, createPipelineFn) {
    const hybridSearchPipeline = createPipelineFn(inputPipelines);

    // Check if any of the input pipelines are mongot input pipelines.
    let hasMongotPipeline = false;
    for (const pipeline of Object.values(inputPipelines)) {
        if (pipeline.length > 0 && mongotInputPipelines.has(pipeline[0])) {
            hasMongotPipeline = true;
            break;
        }
    }

    for (let i = 0; i < views.length; i++) {
        const searchView = views[i];

        // If any part of the input pipeline has a mongot stage, then the hybrid search should fail
        // as mongot queries on mongot views are not allowed.
        if (hasMongotPipeline) {
            assert.commandFailedWithCode(
                searchView.runCommand("aggregate", {pipeline: hybridSearchPipeline, cursor: {}}),
                10623000,
            );
            assert.commandFailedWithCode(
                searchView.runCommand("aggregate", {pipeline: hybridSearchPipeline, explain: true, cursor: {}}),
                10623000,
            );
        } else {
            const hybridSearchPipelineWithViewPrepended = createPipelineFn(inputPipelines, viewPipelines[i]);

            const expectedResultsNoSearchIndexOnView = coll.aggregate(hybridSearchPipelineWithViewPrepended);

            assert.commandWorked(
                coll.runCommand("aggregate", {
                    pipeline: hybridSearchPipelineWithViewPrepended,
                    explain: true,
                    cursor: {},
                }),
            );

            const viewResultsNoSearchIndexOnColl = searchView.aggregate(hybridSearchPipeline);

            assert.commandWorked(
                searchView.runCommand("aggregate", {pipeline: hybridSearchPipeline, explain: true, cursor: {}}),
            );

            if (checkCorrectness) {
                assertDocArrExpectedFuzzy(
                    expectedResultsNoSearchIndexOnView.toArray(),
                    viewResultsNoSearchIndexOnColl.toArray(),
                );
            }
        }
    }
}

export function runHybridSearchWithAllMongotInputPipelinesOnSearchViewsTest(inputPipelines, createPipelineFn) {
    const hybridSearchPipeline = createPipelineFn(inputPipelines);
    for (let i = 0; i < views.length; i++) {
        const searchView = views[i];
        // Creating a search index on a view defined with search should fail with the error code
        // 10623000 because it is illegal to create a search index on a view defined with a search
        // stage.
        assert.commandFailedWithCode(
            searchView.runCommand({createSearchIndexes: viewNames[i], indexes: [searchIndexDef]}),
            10623000,
        );

        // Running a hybrid search query with mongot input pipelines aggregation query should fail
        // on views defined with search.
        assert.commandFailedWithCode(
            searchView.runCommand("aggregate", {pipeline: hybridSearchPipeline, cursor: {}}),
            10623000,
        );

        // Explain for this query should fail.
        assert.commandFailedWithCode(
            searchView.runCommand("aggregate", {pipeline: hybridSearchPipeline, explain: true, cursor: {}}),
            10623000,
        );
    }
}

dropSearchIndex(coll, {name: searchIndexFoo});
dropSearchIndex(coll, {name: vectorSearchIndexV});
