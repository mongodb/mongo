/**
 * Utility functions for explain() results of search + non-search queries run on a view.
 */

import {
    getAggPlanStages,
} from "jstests/libs/analyze_plan.js";
import {
    validateMongotStageExplainExecutionStats,
    verifyShardsPartExplainOutput
} from "jstests/with_mongot/common_utils.js";

function assertIdLookupContainsViewPipeline(explainStages, viewPipeline) {
    assert(explainStages[1].hasOwnProperty("$_internalSearchIdLookup"));
    assert(explainStages[1]["$_internalSearchIdLookup"].hasOwnProperty("subPipeline"));
    let idLookupFullSubPipe = explainStages[1]["$_internalSearchIdLookup"]["subPipeline"];
    // The _idLookup subPipeline should be a $match on _id followed by the view stages.
    let idLookupStage = {"$match": {"_id": {"$eq": "_id placeholder"}}};
    assert.eq(idLookupFullSubPipe[0], idLookupStage);
    // Make sure that idLookup subpipeline contains all of the view stages.
    let idLookupViewStages = idLookupFullSubPipe.slice(-(idLookupFullSubPipe.length - 1));
    assert.eq(idLookupViewStages.length, viewPipeline.length);
    for (let i = 0; i < idLookupViewStages.length; i++) {
        let stageName = Object.keys(viewPipeline[i])[0];
        assert(idLookupViewStages[i].hasOwnProperty(stageName));
    }
}

function assertToplevelAggContainsView(explainStages, viewPipeline) {
    for (let i = 0; i < viewPipeline.length; i++) {
        let stageName = Object.keys(viewPipeline[i])[0];
        assert(explainStages[i].hasOwnProperty(stageName));
    }
}

/**
 * If the top-level aggregation contains a mongot stage, it asserts that the view transforms are
 * contained in _idLookup's subpipeline.  If the top-level aggregation doesn't have a mongot stage,
 * it asserts that the view stages were applied to the beginning of the top-level pipeline.
 * @param {Array} explainStages The list of stages returned from explain().
 * @param {Array} userPipeline The request/query that was run on the view.
 * @param {Object} viewPipeline The pipeline used to define the view.
 */
export function assertViewAppliedCorrectly(explainStages, userPipeline, viewPipeline) {
    if (userPipeline[0].hasOwnProperty("$search") ||
        userPipeline[0].hasOwnProperty("$vectorSearch")) {
        return assertIdLookupContainsViewPipeline(explainStages, viewPipeline);
    }
    return assertToplevelAggContainsView(explainStages, viewPipeline);
}

/**
 * This function checks that the explain output for $search queries from an e2e test contains the
 * information that it should.
 * @param {Object} explainOutput the results from running coll.explain().aggregate([[$search: ....],
 *     ...])
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$_internalSearchIdLookup "
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 * @param {NumberLong} nReturned not needed if verbosity is 'queryPlanner'. For a sharded scenario,
 *     this should be the total returned across all shards.
 */
export function verifyE2ESearchExplainOutput(
    {explainOutput, stageType, verbosity, nReturned = null}) {
    if (explainOutput.hasOwnProperty("splitPipeline")) {
        // We check metadata and protocol version for sharded $search.
        verifyShardsPartExplainOutput({result: explainOutput, searchType: "$search"});
    }
    let totalNReturned = 0;
    let stages = getAggPlanStages(explainOutput, stageType);
    assert(stages.length > 0,
           "There should be at least one stage corresponding to " + stageType +
               " in the explain output. " + tojson(explainOutput));
    // In a sharded scenario, there may be multiple stages. For an unsharded scenario, there is
    // only one stage.
    for (let stage of stages) {
        if (verbosity != "queryPlanner") {
            assert(stage.hasOwnProperty("nReturned"));
            assert(stage.hasOwnProperty("executionTimeMillisEstimate"));
            totalNReturned += stage["nReturned"];
        }
        // Non $_internalSearchIdLookup stages must contain an explain object.
        if (stageType != "$_internalSearchIdLookup") {
            const explainStage = stage[stageType];
            assert(explainStage.hasOwnProperty("explain"), explainStage);
        }
    }
    if (verbosity != "queryPlanner") {
        assert.eq(totalNReturned, nReturned);
    }
}

/**
 * This function checks that the explain output for $searchMeta queries from an e2e test contains
 * the information that it should.
 * @param {Object} explainOutput the results from running coll.explain().aggregate([[$searchMeta:
 *     ....], ...])
 * @param {NumberLong} numFacetBucketsAndCount The number of documents mongot returns for the query
 *     in the sharded scenario. This should be the number of facet buckets involved in the query and
 *     one more for count.
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 */
export function verifyE2ESearchMetaExplainOutput(
    {explainOutput, numFacetBucketsAndCount, verbosity}) {
    // In an unsharded scenario, $searchMeta returns one document with all of the facet values.
    let nReturned = 1;
    if (explainOutput.hasOwnProperty("splitPipeline")) {
        // We check metadata and protocol version for sharded $search.
        verifyShardsPartExplainOutput({result: explainOutput, searchType: "$searchMeta"});
        // In the sharded scenario, $searchMeta returns one document per facet bucket + count, as it
        // needs to be merged in the merging pipeline.
        nReturned = numFacetBucketsAndCount;
    }
    let stages = getAggPlanStages(explainOutput, "$searchMeta");
    for (let stage of stages) {
        validateMongotStageExplainExecutionStats({
            stage: stage,
            stageType: "$searchMeta",
            nReturned: nReturned,
            verbosity: verbosity,
            isE2E: true
        });
    }
}

/**
 * Generates an array of random values from [-1, 1). Useful for explain when the actual documents
 * and scoring for $vectorSearch does not matter.
 *
 * @param {*} n length of array
 * @returns array of random values from [-1, 1]
 */
export function generateRandomVectorEmbedding(n) {
    // Generates array of random values from [-1, 1)
    let embedding = Array.from({length: n}, function() {
        return Math.random() * 2 - 1;
    });
    return embedding;
}

/**
 * This function checks that the explain output for $vectorSearch queries from an e2e test contains
 * the information that it should.
 *
 * In the sharded scenario, make sure the collection has enough documents to return the limit for
 * each shard.
 * @param {Object} explainOutput the results from running coll.explain().aggregate([[$vectorSearch:
 *     ....], ...])
 * @param {string} stageType ex. "$vectorSearch", $_internalSearchMongotRemote" ,
 * @param {NumberLong} limit The limit for the $vectorSearch query. In a sharded scenario, this
 *     applies per shard.
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 */
export function verifyE2EVectorSearchExplainOutput({explainOutput, stageType, limit, verbosity}) {
    let stages = getAggPlanStages(explainOutput, stageType);
    // For $vectorSearch, the limit in the query is applied per shard. This means that nReturned for
    // each shard should match the limit. We don't need to differentiate between the sharded and
    // unsharded scenario.
    for (let stage of stages) {
        validateMongotStageExplainExecutionStats({
            stage: stage,
            stageType: stageType,
            nReturned: limit,
            verbosity: verbosity,
            isE2E: true
        });
    }
}
