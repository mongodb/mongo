/**
 * Utility functions for testing scoreDetails.
 */

import {
    getMoviePlotEmbeddingById,
    makeMovieSearchQuery,
    makeMovieVectorCandidatesQuery,
} from "jstests/with_mongot/e2e_lib/data/movies.js";

// ----------------$vectorSearch stage constants----------------
const limit = 20;
const vectorSearchOverrequestFactor = 10;

// ---------------------Aggregation stages----------------------
// These stages are used to build input pipelines to hybrid search
// stages. Their specifications refer to the 'movies' dataset.
export const vectorStage = makeMovieVectorCandidatesQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: limit,
    numCandidates: limit * vectorSearchOverrequestFactor,
});
export const searchStageWithDetails = makeMovieSearchQuery({queryString: "ape", scoreDetails: true});
export const searchStageNoDetails = makeMovieSearchQuery({queryString: "ape", scoreDetails: false});
export const sortAscendingStage = {
    $sort: {_id: 1},
};
export const projectScoreAndScoreDetailsStage = {
    $project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}},
};
export const projectScoreStage = {
    $project: {score: {$meta: "score"}},
};
export const projectOutPlotEmbeddingStage = {
    $project: {plot_embedding: 0},
};
export const limitStage = {
    $limit: limit,
};

// ---------------------Description strings---------------------
export const scoreFusionScoreDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across input " +
    "pipelines from which this document is output from:";
export const rankFusionScoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + " +
    "rank))) across input pipelines from which this document is output, from:";
export const scoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized " +
    "or nonnormalized value:";

// ----------------------Utility functions----------------------
export function calculateReciprocalRankFusionScore(weight, rank) {
    return weight * (1 / (60 + rank));
}

export function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

/**
 * All input pipelines should contain the following fields when $rankFusion's scoreDetails is
 * enabled: inputPipelineName, rank, and weight. Only inputPipelineName's and weight's values are
 * constant across the results.
 *
 * Example of the expected document structure:
 * {
 *      "inputPipelineName": "pipeline1",
 *      "rank": 1,
 *      "weight": 0.5,
 *  }
 */
export function checkDefaultPipelineScoreDetails(stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    assertFieldPresent("inputPipelineName", subDetails);
    assert.eq(subDetails["inputPipelineName"], pipelineName);
    if (stageType === "rank") {
        assertFieldPresent("rank", subDetails);
    }
    assertFieldPresent("weight", subDetails);
    assert.eq(subDetails["weight"], weight);
}

/**
 * Check the default scoreDetails fields output from a $score stage.
 *
 * Example of the expected document structure:
 * "score": 0.05555555555555555,
 * "details": {
 *      "value": 0.05555555555555555,
 *      "description": "the score calculated from...",
 *      "rawScore": 6,
 *      "normalization": "minMaxScaler", // ["none", "sigmoid", "minMaxScaler"]
 *      "weight": 0.5, // between [0,1]
 *      "expression": "{ string: { $add: [ '$single', '$double' ] } }", // can be null.
 *      "details": []
 *  }
 *
 * Example of 'scoreDetailsSpec':
 * {
 *    value: 0.8, // floating point number.
 *    rawScore: 1.5, // floating point number.
 *    description: 'the score calculated...', // can be null.
 *    weight: 0.5, // floating point number in range [0, 1] or null.
 *    normalization: 'none', // 'none', 'minMaxScaler', 'sigmoid' or null.
 *    expression: "{ string: { $add: [ '$single', '$double' ] } }" // string that describes the
 *                                                                 // score expression; can be null.
 * }
 */
export function checkScoreScoreDetails(foundDoc, scoreDetailsSpec) {
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert.eq(score, scoreDetailsSpec["value"]);
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    assert(fieldPresent("rawScore", details), details);
    assert.eq(details["rawScore"], scoreDetailsSpec["rawScore"]);
    if (scoreDetailsSpec.hasOwnProperty("normalization") && scoreDetailsSpec["normalization"] !== "none") {
        assert(fieldPresent("normalization", details), details);
        assert.eq(details["normalization"], scoreDetailsSpec["normalization"]);
    }
    if (scoreDetailsSpec.hasOwnProperty("expression") && scoreDetailsSpec["expression"] !== null) {
        assert(fieldPresent("expression", details), details);
        assert.eq(details["expression"], scoreDetailsSpec["expression"]);
    }
    if (scoreDetailsSpec.hasOwnProperty("description") && scoreDetailsSpec["description"] !== null) {
        assert(fieldPresent("description", details), details);
        assert.eq(details["description"], scoreDetailsSpec["description"]);
    }
    if (scoreDetailsSpec.hasOwnProperty("weight") && scoreDetailsSpec["weight"] !== null) {
        assert(fieldPresent("weight", details), details);
        assert.eq(details["weight"], scoreDetailsSpec["weight"]);
    }
    assert(fieldPresent("details", details), details);
    assert.eq(details["details"], []);
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight) for a $search input pipeline. If a
 * document was output from the input pipeline (value field is present in scoreDetails), then check
 * that the value and details fields are present. If the search input pipeline has scoreDetails
 * enabled, check the description field is accurate and that the pipeline's scoreDetails aren't
 * empty. Returns the weighted score of the pipelines whose details were provided.
 *
 * Example of the expected document structure:
 * {
 *      "inputPipelineName": "searchPipeline",
 *      "value": 0.05555555555555555,
 *      "inputPipelineRawScore": 6,
 *      "weight": 0.5, // between [0,1]
 *      "details": []
 * }
 */
export function checkSearchScoreDetails(
    stageType,
    assertFieldPresent,
    subDetails,
    pipelineName,
    weight,
    isScoreDetails,
) {
    assertFieldPresent("inputPipelineName", subDetails);
    assert.eq(subDetails["inputPipelineName"], pipelineName);
    if (stageType === "rank") {
        assertFieldPresent("rank", subDetails);
    }

    const cameFromSearch = subDetails.hasOwnProperty("value") && subDetails["value"] !== 0;
    if (!cameFromSearch) {
        if (stageType === "rank") {
            assert.eq(subDetails["rank"], "NA");
        }
        // If there isn't a value field, that signifies that there was no document output from the
        // $search input pipeline, so there are no scoreDetails to report.
        return 0;
    }

    assertFieldPresent("weight", subDetails);
    assert.eq(subDetails["weight"], weight);
    assertFieldPresent("value", subDetails); // Output of rank calculation.
    assertFieldPresent("details", subDetails);
    if (stageType === "rank") {
        if (isScoreDetails) {
            assertFieldPresent("description", subDetails);
            assert.eq(subDetails["description"], "sum of:");
        } else {
            // Note we won't check the shape of the search scoreDetails beyond here.
            assert.eq(subDetails["details"], []);
        }

        return calculateReciprocalRankFusionScore(subDetails["weight"], subDetails[stageType]);
    }

    return subDetails["weight"] * subDetails["inputPipelineRawScore"];
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a $vectorSearch input
 * pipeline. Note that vectorSearch input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the weighted score of the pipelines whose details were
 * provided.
 *
 * Example of the expected document structure:
 * {
 *      "inputPipelineName": "vectorSearchPipeline",
 *      "value": 0.05555555555555555,
 *      "inputPipelineRawScore": 6,
 *      "weight": 0.5, // between [0,1]
 *      "details": []
 * }
 */
export function checkVectorScoreDetails(stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(stageType, assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("value", subDetails); // Original 'score' AKA vectorSearchScore.
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    let vectorSearchScore =
        stageType === "rank"
            ? calculateReciprocalRankFusionScore(subDetails["weight"], subDetails[stageType])
            : subDetails["weight"] * subDetails["inputPipelineRawScore"];
    return vectorSearchScore;
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a $geoNear input
 * pipeline. Note that geoNear input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the weighted score of the pipelines whose details were
 * provided.
 *
 * Example of the expected document structure:
 * {
 *      "inputPipelineName": "geoNearPipeline",
 *      "inputPipelineRawScore": 6,
 *      "weight": 0.5, // between [0,1]
 *      "details": []
 * }
 */
export function checkGeoNearScoreDetails(stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(stageType, assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    let geoNearScore =
        stageType === "rank"
            ? calculateReciprocalRankFusionScore(subDetails["weight"], subDetails[stageType])
            : subDetails["weight"] * subDetails["inputPipelineRawScore"];
    return geoNearScore;
}

/**
 * For each document or result, check the following fields in the outer scoreDetails: score,
 * details, value, description, and that the subDetails array contains 1 entry for each input
 * pipeline. The contents of the subDetails array are not checked here, but can be checked by
 * calling the other utility functions in this file.
 *
 * Example of the expected document structure:
 * "score": 0.05555555555555555,
 * "details": {
 *      "value": 0.05555555555555555,
 *      "description": "the score calculated from...",
 *      "rawScore": 6,
 *      "normalization": "minMaxScaler", // ["none", "sigmoid", "minMaxScaler"]
 *      "combination": {
 *          "method": "average", // ["average", "custom expression"]
 *      },
 *      "weight": 0.5, // between [0,1]
 *      "expression": "{ string: { $add: [ '$single', '$double' ] } }", // can be null.
 *      "details": [...] // Array of sub-details, one for each input pipeline.
 *  }
 *
 * Example of 'scoreDetailsSpec':
 * {
 *    stageType: 'score', // 'score' or 'rank'.
 *    numInputPipelines: 2,
 *    normalization: 'none', // 'none', 'minMaxScaler', 'sigmoid' or null.
 *    combinationMethod: 'avg', // 'avg', 'expression' or null.
 *    combinationExpressionString: null, // only present if 'combinationMethod' is 'expression'.
 * }
 */
export function checkOuterScoreDetails(foundDoc, scoreDetailsSpec) {
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    assert(fieldPresent("description", details), details);
    if (scoreDetailsSpec["stageType"] == "score") {
        assert.eq(details["description"], scoreFusionScoreDetailsDescription);
        assert.eq(details["normalization"], scoreDetailsSpec["normalization"]);
        const combination = details["combination"];
        assert(fieldPresent("method", combination), combination);
        if (scoreDetailsSpec["combinationMethod"] === "expression") {
            assert.eq(combination["method"], "custom expression");
            assert(fieldPresent("expression", combination), combination);
            // Assert that the stringified custom expression is correct.
            assert.eq(combination["expression"], scoreDetailsSpec["combinationExpressionString"]);
        } else if (scoreDetailsSpec["combinationMethod"] === "avg") {
            assert.eq(combination["method"], "average");
        }
    } else {
        assert.eq(details["description"], rankFusionScoreDetailsDescription);
    }

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj), `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }

    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, scoreDetailsSpec["numInputPipelines"]);

    return [assertFieldPresent, subDetails, score];
}
