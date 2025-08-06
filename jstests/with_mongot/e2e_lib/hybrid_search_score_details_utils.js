/**
 * Utility functions for testing scoreDetails.
 */

import {
    getMoviePlotEmbeddingById,
    makeMovieSearchQuery,
    makeMovieVectorCandidatesQuery
} from "jstests/with_mongot/e2e_lib/data/movies.js";

// ----------------$vectorSearch stage constants----------------
const limit = 20;
const vectorSearchOverrequestFactor = 10;

// ---------------------Aggregation stages----------------------
export const vectorStage = makeMovieVectorCandidatesQuery({
    queryVector: getMoviePlotEmbeddingById(6),
    limit: limit,
    numCandidates: limit * vectorSearchOverrequestFactor
});
export const searchStageWithDetails =
    makeMovieSearchQuery({queryString: "ape", scoreDetails: true});
export const searchStageNoDetails = makeMovieSearchQuery({queryString: "ape", scoreDetails: false});
export const sortAscendingStage = {
    $sort: {_id: 1}
};
export const projectScoreAndScoreDetailsStage = {
    $project: {score: {$meta: "score"}, details: {$meta: "scoreDetails"}}
};
export const projectScoreStage = {
    $project: {score: {$meta: "score"}}
};
export const projectOutPlotEmbeddingStage = {
    $project: {plot_embedding: 0}
};
export const limitStage = {
    $limit: limit
};

// ---------------------Description strings---------------------
export const scoreFusionScoreDetailsDescription =
    "the value calculated by combining the scores (either normalized or raw) across input " +
    "pipelines from which this document is output from:";
export const rankFusionScoreDetailsDescription =
    "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + " +
    "rank))) across input pipelines from which this document is output, from:";
export const scoreDetailsDescription =
    "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:";

// ----------------------Utility functions----------------------
export function calculateReciprocalRankFusionScore(weight, rank) {
    return (weight * (1 / (60 + rank)));
};

export function fieldPresent(field, containingObj) {
    return containingObj.hasOwnProperty(field);
}

/**
 * All input pipelines should contain the following fields when $rankFusion's scoreDetails is
 * enabled: inputPipelineName, rank, and weight. Only inputPipelineName's and weight's values are
 * constant across the results.
 */
export function checkDefaultPipelineScoreDetails(
    stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    assertFieldPresent("inputPipelineName", subDetails);
    assert.eq(subDetails["inputPipelineName"], pipelineName);
    if (stageType === "rank") {
        assertFieldPresent("rank", subDetails);
    }
    assertFieldPresent("weight", subDetails);
    assert.eq(subDetails["weight"], weight);
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight) for a search input pipeline. If a
 * document was output from the input pipeline (value field is present in scoreDetails), then check
 * that the value and details fields are present. If the search input pipeline has scoreDetails
 * enabled, check the description field is accurate and that the pipeline's scoreDetails aren't
 * empty. Returns the RRF score for this input pipeline.
 */
export function checkSearchScoreDetails(
    stageType, assertFieldPresent, subDetails, pipelineName, weight, isScoreDetails) {
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
    assertFieldPresent("value", subDetails);  // Output of rank calculation.
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
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a vectorSearch input
 * pipeline. Note that vectorSearch input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the RRF score for this input pipeline.
 */
export function checkVectorScoreDetails(
    stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(
        stageType, assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("value", subDetails);  // Original 'score' AKA vectorSearchScore.
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    let vectorSearchScore = stageType === "rank"
        ? calculateReciprocalRankFusionScore(subDetails["weight"], subDetails[stageType])
        : subDetails["weight"] * subDetails["inputPipelineRawScore"];
    return vectorSearchScore;
}

/**
 * Checks the scoreDetails (inputPipelineName, rank, weight, details) for a geoNear input
 * pipeline. Note that geoNear input pipeline do not have scoreDetails so the details field
 * should always be an empty array. Returns the RRF score for this input pipeline.
 */
export function checkGeoNearScoreDetails(
    stageType, assertFieldPresent, subDetails, pipelineName, weight) {
    checkDefaultPipelineScoreDetails(
        stageType, assertFieldPresent, subDetails, pipelineName, weight);
    assertFieldPresent("details", subDetails);
    assert.eq(subDetails["details"], []);
    let geoNearScore = stageType === "rank"
        ? calculateReciprocalRankFusionScore(subDetails["weight"], subDetails[stageType])
        : subDetails["weight"] * subDetails["inputPipelineRawScore"];
    return geoNearScore;
}

/**
 * For each document or result, check the following fields in the outer scoreDetails: score,
 * details, value, description, and that the subDetails array contains 1 entry for each input
 * pipeline.
 */
export function checkOuterScoreDetails(stageType, foundDoc, numInputPipelines) {
    // Assert that the score metadata has been set.
    assert(fieldPresent("score", foundDoc), foundDoc);
    const score = foundDoc["score"];
    assert(fieldPresent("details", foundDoc), foundDoc);
    const details = foundDoc["details"];
    assert(fieldPresent("value", details), details);
    // Assert that the score metadata is the same value as what scoreDetails set.
    assert.eq(details["value"], score);
    assert(fieldPresent("description", details), details);
    assert.eq(details["description"],
              stageType === "rank" ? rankFusionScoreDetailsDescription
                                   : scoreFusionScoreDetailsDescription);

    function assertFieldPresent(field, obj) {
        assert(fieldPresent(field, obj),
               `Looked for ${field} in ${tojson(obj)}. Full details: ${tojson(details)}`);
    }
    // Description of rank fusion. Wrapper on both search / vector.
    assertFieldPresent("details", details);
    const subDetails = details["details"];
    assert.eq(subDetails.length, numInputPipelines);

    return [assertFieldPresent, subDetails, score];
}
