/**
 * Utility functions that are common for both mocked and e2e search tests.
 */

/**
 * Checks that the explain object for a sharded search query contains the correct "protocolVersion".
 * Checks 'metaPipeline' and 'sortSpec' if it's provided.
 * * @param {Object} result the results from running coll.explain().aggregate([[$search: ....],
 * ...])
 * @param {string} searchType ex. "$search", "$searchMeta"
 * @param {Object} metaPipeline
 * @param {NumberInt()} protocolVersion To check the value of the protocolVersion, the value
 * returned from explain will be wrapped with NumberInt(). The default value to check is 1 since
 * mongot currently always returns 1 for its protocolVersion.
 * @param {Object} sortSpec
 */
export function verifyShardsPartExplainOutput({
    result,
    searchType,
    metaPipeline = null,
    protocolVersion = NumberInt(1),
    sortSpec = null,
}) {
    // Checks index 0 of 'shardsPart' since $search, $searchMeta need to come first in the pipeline
    assert(result.splitPipeline.shardsPart[0][searchType].hasOwnProperty(
        "metadataMergeProtocolVersion"));
    assert(result.splitPipeline.shardsPart[0][searchType].hasOwnProperty("mergingPipeline"));

    assert.eq(
        NumberInt(result.splitPipeline.shardsPart[0][searchType].metadataMergeProtocolVersion),
        protocolVersion);

    if (metaPipeline) {
        assert.eq(result.splitPipeline.shardsPart[0][searchType].mergingPipeline, metaPipeline);
    }
    if (sortSpec) {
        assert(result.splitPipeline.shardsPart[0][searchType].hasOwnProperty("sortSpec"));
        assert.eq(result.splitPipeline.shardsPart[0][searchType].sortSpec, sortSpec);
    }
}

/**
 * Helper to create an explain object that getAggPlanStages() can be used on.
 *
 * @param {Object} unionSubExplain The explain output for the $unionWith sub pipeline. Given an
 *     unionWithStage, accessed with unionWithStage.$unionWith.pipeline.
 *
 * @returns explain object that getAggPlanStages() can be called to retrieve stages.
 */

export function prepareUnionWithExplain(unionSubExplain) {
    if (unionSubExplain.hasOwnProperty("stages") || unionSubExplain.hasOwnProperty("shards") ||
        unionSubExplain.hasOwnProperty("queryPlanner")) {
        return unionSubExplain;
    }
    // In the case that the pipeline doesn't have "stages", "shards", or "queryPlanner", the explain
    // output is an array of stages. We return {stages: []} to replicate what a normal explain does.
    assert(
        Array.isArray(unionSubExplain),
        "We expect the input is an array here. If this is not an array, this function needs to be " +
            "updated in order to replicate a non-unionWith explain object. " +
            tojson(unionSubExplain));

    // The first stage of the explain array should be a initial mongot stage.
    assert(Object.keys(unionSubExplain[0]).includes("$_internalSearchMongotRemote") ||
               Object.keys(unionSubExplain[0]).includes("$searchMeta") ||
               Object.keys(unionSubExplain[0]).includes("$vectorSearch"),
           "The first stage of the array should be a mongot stage.");

    return {stages: unionSubExplain};
}

const mongotStages =
    ["$_internalSearchMongotRemote", "$searchMeta", "$_internalSearchIdLookup", "$vectorSearch"];

/**
 * Validates that the mongot stage from the explain output includes the required execution metrics.
 *
 * @param {Object} stage The object representing a mongot stage ($_internalSearchMongotRemote,
 *     $searchMeta, $_internalSearchIdLookup) from explain output.
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$searchMeta",
 *     "$_internalSearchIdLookup", "$vectorSearch"
 * @param {string} verbosity The verbosity of explain. "nReturned" and "executionTimeMillisEstimate"
 *     will not be checked for 'queryPlanner' verbosity "
 * @param {NumberLong} nReturned The number of documents that should be returned in the
 *     mongot stage. Optional for "queryPlanner" verbosity.
 * @param {Object} explain The explain object that the stage should contain.
 * @param {Boolean} isE2E True if this function is being called for an e2e test. Since an e2e test
 *     uses an actual mongot, we don't know the value of the explain object and cannot check it.
 */
export function validateMongotStageExplainExecutionStats(
    {stage, stageType, verbosity, nReturned = null, explain = null, isE2E = false}) {
    assert(mongotStages.includes(stageType),
           "stageType must be a mongot stage found in mongotStages.");
    assert(stage[stageType],
           "Given stage isn't the expected stage. " + stageType + " is not found.");

    const isIdLookup = stageType === "$_internalSearchIdLookup";

    if (verbosity != "queryPlanner") {
        assert(stage.hasOwnProperty("nReturned"));
        assert.eq(nReturned, stage["nReturned"]);
        assert(stage.hasOwnProperty("executionTimeMillisEstimate"));

        if (isIdLookup) {
            const idLookupStage = stage["$_internalSearchIdLookup"];
            assert(idLookupStage.hasOwnProperty("totalDocsExamined"), idLookupStage);
            assert.eq(idLookupStage["totalDocsExamined"], nReturned, idLookupStage);
            assert(idLookupStage.hasOwnProperty("totalKeysExamined"), idLookupStage);
            assert.eq(idLookupStage["totalKeysExamined"], nReturned, idLookupStage);
        }
    }

    // Non $_internalSearchIdLookup mongot stages must contain an explain object.
    if (!isIdLookup) {
        const explainStage = stage[stageType];
        assert(explainStage.hasOwnProperty("explain"), explainStage);
        // We don't know the actual value of the explain object for e2e tests and can't check it.
        if (!isE2E) {
            assert(explain, "Explain is null but needs to be provided for initial mongot stage.");
            assert.eq(explain, explainStage["explain"]);
        }
    }
}
