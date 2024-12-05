/**
 * Duplicate JS tests for some of the edge cases found in
 * src/mongo/db/pipeline/document_source_score_fusion_test.cpp
 *
 * These will be picked up/ran by the js fuzzer suite.
 * @tags: [ featureFlagSearchHybridScoring ]
 */

const collName = "search_score_fusion";

const vectorSearchClause = {pipeline: [{
    $vectorSearch: {
        queryVector: [1.0, 2.0, 3.0],
        path: "plot_embedding",
        numCandidates: 300,
        index: "vector_index",
        limit: 10
    }}], as: "score1"
};

const searchClause = {
    pipeline: [{$search: {index: "search_index", text: {query: "mystery", path: "genres"}}}]
};

const searchMatchAsClause = {pipeline: [{
    $search: {
        index: "search_index", text: {query: "mystery", path: "genres"}}}, {
    $match: {
        author: "dave"
    }}], as: "score1"
};

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// Check that a single input pipeline is allowed.
assert.commandWorked(runPipeline(
    [{$scoreFusion: {inputs: [vectorSearchClause], inputNormalization: "none", weights: []}}]));

// Error if the weights array doesn't have elements of type safe double
assert.commandFailedWithCode(runPipeline([{
                                 $scoreFusion: {
                                     inputs: [searchMatchAsClause],
                                     score: "expression",
                                     inputNormalization: "none",
                                     weights: ["hi"]
                                 }
                             }]),
                             ErrorCodes.TypeMismatch);

// Check that an array of ints for weights is a valid input
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        inputs: [vectorSearchClause, searchClause],
        score: "expression",
        inputNormalization: "none",
        weights: [5, 100]
    }
}]));

// Check that a mixed array of ints/decimals for weights is a valid input
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        inputs: [searchMatchAsClause, searchClause],
        score: "expression",
        inputNormalization: "none",
        weights: [5, 100.2]
    }
}]));

// Check that including all optional arguments is parsed correctly
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        inputs: [searchMatchAsClause],
        score: "expression",
        inputNormalization: "none",
        weights: [5],
        scoreNulls: 0
    }
}]));
