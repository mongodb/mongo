/**
 * Tests that $rankFusion on a view namespace, defined with a search pipeline, is allowed and works
 * correctly.
 *
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

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
    bulk.insert({
        _id: i,
        a: "foo",
        m: "baz",
        x: i / 3,
        loc: [i, i],
        v: [1, 0, 8, 1, 8],
        z: [2, 0, 1, 1, 4]
    });
}
// Populate the odd-indexed documents
for (let i = 1; i < nDocs; i += 2) {
    bulk.insert({_id: i, a: "bar", x: i / 3, loc: [i, i], v: [1, 0, 8, 1, 8], z: [2, -2, 1, 4, 4]});
}
assert.commandWorked(bulk.execute());

const searchIndexFoo = "searchIndexFoo";
const vectorSearchIndexV = "vectorSearchIndexV";

createSearchIndex(coll, {name: searchIndexFoo, definition: {"mappings": {"dynamic": true}}});

createSearchIndex(coll, {
    name: vectorSearchIndexV,
    type: "vectorSearch",
    definition:
        {"fields": [{"type": "vector", "numDimensions": 5, "path": "v", "similarity": "euclidean"}]}
});

const searchPipelineFoo = () => {
    return {$search: {index: searchIndexFoo, text: {query: "foo", path: "a"}}};
};

const searchPipelineBar = () => {
    return {$search: {index: "searchIndexBar", text: {query: "bar", path: "m"}}};
};

const vectorSearchPipelineV = () => {
    return {
        $vectorSearch: {
            queryVector: [1, 0, 8, 1, 8],
            path: "v",
            numCandidates: nDocs,
            index: vectorSearchIndexV,
            limit: nDocs,
        }
    };
};

const vectorSearchPipelineZ = () => {
    return {
        $vectorSearch: {
            queryVector: [2, 0, 3, 1, 4],
            path: "z",
            numCandidates: nDocs,
            index: "vectorSearchIndexZ",
            limit: nDocs,
        }
    };
};

const searchIndexName = jsTestName() + "_index";
const searchIndexDef = {
    name: searchIndexName,
    definition: {mappings: {dynamic: true}}
};

/**
 * This function creates a $rankFusion pipeline with the provided input pipelines.
 *
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 */
const createRankFusionPipeline = (inputPipelines) => {
    const rankFusionStage = {$rankFusion: {input: {pipelines: {}}}};
    for (const [key, pipeline] of Object.entries(inputPipelines)) {
        // Otherwise, just use the input pipeline as is.
        rankFusionStage.$rankFusion.input.pipelines[key] = [...pipeline];
    }

    return [rankFusionStage];
};

// run traceExceptions = true
const createViews = () => {
    const viewPipelineNames = ["searchNoIndex", "searchWithIndex", "vectorSearch"];

    // Note that the first pipeline doesn't specify a searchIndex while the second one does.
    const viewPipelines = [
        [{$search: {text: {query: "foo", path: "a"}}}],
        [searchPipelineFoo(searchIndexName)],
        [vectorSearchPipelineV(searchIndexName)]
    ];

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
/**
 * This function creates a $rankFusion pipeline with the provided input pipelines and runs it on
 * views defined with various search pipelines. Asserts that the expected behavior is realized when
 * running aggregations on the view. Assuming that all input pipelines are search pipelines, no
 * search results should be returned so that's what's checked for.
 *
 * @param {string} testName name of the test, used to create a unique view.
 * @param {object} inputPipelines spec for $rankFusion input pipelines; can be as many as needed.
 */
const runRankFusionOnSearchViewsTest = (testName, inputPipelines) => {
    const rankFusionPipeline = createRankFusionPipeline(inputPipelines);
    for (let i = 0; i < views.length; i++) {
        const searchView = views[i];
        // Creating a search index on a view defined with search should fail with the error code
        // 10623000.
        assert.commandFailedWithCode(
            searchView.runCommand({createSearchIndexes: viewNames[i], indexes: [searchIndexDef]}),
            10623000);

        // Running a $rankFusion with mongot input pipelines aggregation query should work on views
        // defined with search.
        assert.commandWorked(
            searchView.runCommand("aggregate", {pipeline: rankFusionPipeline, cursor: {}}));

        // Check explain query works running a rank fusion on a search view
        assert.commandWorked(searchView.runCommand(
            "aggregate", {pipeline: rankFusionPipeline, explain: true, cursor: {}}));

        // If all of the $rankFusion query's input pipelines are mongot input pipelines, then the
        // aggregation query should fail silently (not return any documents).
        assert.eq(searchView.aggregate(rankFusionPipeline)["_batch"], []);
    }
};

runRankFusionOnSearchViewsTest("only_search", {a: [searchPipelineFoo()]});
runRankFusionOnSearchViewsTest("only_vector_search", {a: [vectorSearchPipelineV()]});
runRankFusionOnSearchViewsTest("double_search", {
    a: [searchPipelineFoo()],
    b: [searchPipelineBar()],
});
runRankFusionOnSearchViewsTest("swapped_double_search", {
    a: [searchPipelineBar()],
    b: [searchPipelineFoo()],
});
runRankFusionOnSearchViewsTest("double_vector_search", {
    a: [vectorSearchPipelineV()],
    b: [vectorSearchPipelineZ()],
});
runRankFusionOnSearchViewsTest("swapped_double_vector_search", {
    a: [vectorSearchPipelineZ()],
    b: [vectorSearchPipelineV()],
});
runRankFusionOnSearchViewsTest("multi_search", {
    a: [searchPipelineBar()],
    b: [vectorSearchPipelineV()],
});
runRankFusionOnSearchViewsTest("swapped_multi_search", {
    a: [vectorSearchPipelineV()],
    b: [searchPipelineBar()],
});
dropSearchIndex(coll, {name: searchIndexFoo});
dropSearchIndex(coll, {name: vectorSearchIndexV});
