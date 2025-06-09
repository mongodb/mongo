/**
 * Duplicate JS tests for some of the edge cases found in
 * src/mongo/db/pipeline/document_source_score_fusion_test.cpp
 *
 * These will be picked up/ran by the js fuzzer suite.
 *
 * TODO SERVER-100404: Re-enable this for sharded queries.
 * @tags: [
 *   assumes_unsharded_collection,
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_81
 * ]
 */

const collName = "search_score_fusion";
const coll = db[collName];

const vectorSearchClauseAndSearchClause = {
    score1: [{
        $vectorSearch: {
            queryVector: [1.0, 2.0, 3.0],
            path: "plot_embedding",
            numCandidates: 300,
            index: "vector_index",
            limit: 10
        }
    }],
    search1: [{$search: {index: "search_index", text: {query: "mystery", path: "genres"}}}]
};

const searchMatchAsClause = {
    score2: [
        {$search: {index: "search_index", text: {query: "mystery", path: "genres"}}},
        {$match: {author: "dave"}}
    ]
};

const scoreInputPipelines = {
    score3: [{$match: {author: "Agatha Christie"}}, {$score: {score: 50.0}}],
    score4: [{$match: {author: "dave"}}, {$score: {score: 10.0}}]
};

const textMatchClause = {
    score5: [{$match: {$text: {$search: "coffee"}}}]
};

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

// Check that a single input pipeline is allowed.
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        input: {pipelines: searchMatchAsClause, normalization: "none"},
        combination: {weights: {score2: 5}}
    }
}]));

// Error if the weights array doesn't have elements of type safe double
assert.commandFailedWithCode(runPipeline([{
                                 $scoreFusion: {
                                     input: {pipelines: searchMatchAsClause, normalization: "none"},
                                     combination: {weights: {score2: "hi"}}
                                 }
                             }]),
                             13118);

// Check that an array of ints for weights is a valid input
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        input: {pipelines: vectorSearchClauseAndSearchClause, normalization: "none"},
        combination: {weights: {score1: 5, search1: 100}}
    }
}]));

// Check that a mixed array of ints/decimals for weights is a valid input
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        input: {pipelines: vectorSearchClauseAndSearchClause, normalization: "none"},
        combination: {weights: {score1: 5, search1: 100.2}}
    }
}]));

// Check that including all optional arguments is parsed correctly
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        input: {pipelines: searchMatchAsClause, normalization: "none"},
        combination: {weights: {score2: 5}}
    }
}]));

// Check that invalid normalization option throws BadValue error. The correct spelling or value
// is "minMaxScaler" with an e, not "minMaxScalar" with an a.
assert.commandFailedWithCode(
    runPipeline([{
        $scoreFusion: {
            input: {pipelines: scoreInputPipelines, normalization: "minMaxScalar"},
            combination: {weights: {score3: 5, score4: 10}}
        }
    }]),
    ErrorCodes.BadValue);

// Check that sigmoid option parsed correctly
assert.commandWorked(runPipeline([{
    $scoreFusion: {
        input: {pipelines: scoreInputPipelines, normalization: "sigmoid"},
        combination: {weights: {score3: 5, score4: 10}}
    }
}]));

// Check that $text match is valid.
assert.commandWorked(coll.createIndex({text: "text"}));
assert.commandWorked(
    runPipeline([{$scoreFusion: {input: {pipelines: textMatchClause, normalization: "none"}}}]));

// Check that unscored pipeline is invalid.
assert.commandFailedWithCode(
    runPipeline([{
        $scoreFusion: {
            input: {pipelines: {pipeOne: [{$limit: 2}]}, normalization: "none"},
        }
    }]),
    9402500);

// Check that non-selection pipeline is invalid
assert.commandFailedWithCode(
    runPipeline([{
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [{$score: {score: 2, normalization: "none"}}, {$project: {score3: 1}}]
                },
                normalization: "none"
            },
        }
    }]),
    9402502);

// TODO: SERVER-104730 add tests for nested $scoreFusion/$rankFusion
