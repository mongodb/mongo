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

    // The first stage of the explain array should be a $search stage.
    assert(Object.keys(unionSubExplain[0]).includes("$_internalSearchMongotRemote") ||
               Object.keys(unionSubExplain[0]).includes("$searchMeta") ||
               Object.keys(unionSubExplain[0]).includes("$vectorSearch"),
           "The first stage of the array should be a search stage.");

    return {stages: unionSubExplain};
}
