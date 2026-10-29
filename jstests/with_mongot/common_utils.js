/**
 * Utility functions that are common for both mocked and e2e search tests.
 */

// Internal DRM container that wraps extension `$search` when featureFlagSearchExtension is on.
export const kDocumentResultsAndMetadataStage = "$_internalDocumentResultsAndMetadata";

export function isMongotProducerStageName(stageName) {
    return (
        stageName === "$_internalSearchMongotRemote" ||
        stageName === kDocumentResultsAndMetadataStage ||
        stageName === "$searchMeta" ||
        stageName === "$vectorSearch"
    );
}

// Extracts the $search/$searchMeta stage from shardsPart[0], unwrapping the extension DRM and
// $_extensionSearchMeta shapes. Returns null if not found.
export function getShardsPartSearchStage(shardsPart0, searchType) {
    if (shardsPart0.hasOwnProperty(searchType)) {
        return shardsPart0[searchType];
    }
    // Extension `$search` is nested under DRM's source.
    if (searchType === "$search" && shardsPart0.hasOwnProperty(kDocumentResultsAndMetadataStage)) {
        const source = shardsPart0[kDocumentResultsAndMetadataStage].source;
        if (source && source.hasOwnProperty(searchType)) {
            return source[searchType];
        }
    }
    // Extension `$searchMeta` serializes as the internal extension stage name.
    if (searchType === "$searchMeta" && shardsPart0.hasOwnProperty("$_extensionSearchMeta")) {
        return shardsPart0["$_extensionSearchMeta"];
    }
    return null;
}

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
    // Checks index 0 of 'shardsPart' since $search, $searchMeta need to come first in the pipeline.
    const searchStage = getShardsPartSearchStage(result.splitPipeline.shardsPart[0], searchType);
    assert(
        searchStage,
        "Expected shardsPart[0] to contain " +
            searchType +
            " (possibly nested under " +
            kDocumentResultsAndMetadataStage +
            " or as $_extensionSearchMeta): " +
            tojson(result.splitPipeline.shardsPart[0]),
    );

    // Legacy stages embed merge metadata on the stage; extension DPL uses splitPipeline.mergerPart.
    if (searchStage.hasOwnProperty("metadataMergeProtocolVersion")) {
        assert(searchStage.hasOwnProperty("mergingPipeline"), searchStage);
        assert.eq(NumberInt(searchStage.metadataMergeProtocolVersion), protocolVersion);
        if (metaPipeline) {
            assert.eq(searchStage.mergingPipeline, metaPipeline);
        }
        if (sortSpec) {
            assert(searchStage.hasOwnProperty("sortSpec"), searchStage);
            assert.eq(searchStage.sortSpec, sortSpec);
        }
    } else {
        assert(
            Array.isArray(result.splitPipeline.mergerPart),
            "Extension sharded search explain should include mergerPart: " + tojson(result),
        );
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
    if (
        unionSubExplain.hasOwnProperty("stages") ||
        unionSubExplain.hasOwnProperty("shards") ||
        unionSubExplain.hasOwnProperty("queryPlanner")
    ) {
        return unionSubExplain;
    }
    // In the case that the pipeline doesn't have "stages", "shards", or "queryPlanner", the explain
    // output is an array of stages. We return {stages: []} to replicate what a normal explain does.
    assert(
        Array.isArray(unionSubExplain),
        "We expect the input is an array here. If this is not an array, this function needs to be " +
            "updated in order to replicate a non-unionWith explain object. " +
            tojson(unionSubExplain),
    );

    // The first stage of the explain array should be an initial mongot stage.
    assert(
        isMongotProducerStageName(Object.keys(unionSubExplain[0])[0]),
        "The first stage of the array should be a mongot stage.",
    );

    return {stages: unionSubExplain};
}

const mongotStages = [
    "$_internalSearchMongotRemote",
    kDocumentResultsAndMetadataStage,
    "$searchMeta",
    "$_internalSearchIdLookup",
    "$vectorSearch",
];

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
 * @param {NumberLong} numFiltered optional, The number of documents filtered out by the idLookup
 *     stage.
 */
export function validateMongotStageExplainExecutionStats({
    stage,
    stageType,
    verbosity,
    nReturned = null,
    explain = null,
    isE2E = false,
    numFiltered = null,
}) {
    assert(
        mongotStages.includes(stageType),
        "stageType must be a mongot stage found in mongotStages.",
    );
    assert(
        stage[stageType],
        "Given stage isn't the expected stage. " + stageType + " is not found.",
    );

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

    // Non $_internalSearchIdLookup mongot stages must contain an explain object. Extension DRM
    // nests it under source.$search.explain.
    if (!isIdLookup) {
        const explainStage = stage[stageType];
        if (stageType === kDocumentResultsAndMetadataStage) {
            const sourceSearch =
                explainStage.source && explainStage.source.$search
                    ? explainStage.source.$search
                    : null;
            assert(
                sourceSearch && sourceSearch.hasOwnProperty("explain"),
                "Expected DRM source.$search.explain: " + tojson(explainStage),
            );
            if (!isE2E) {
                assert(
                    explain,
                    "Explain is null but needs to be provided for initial mongot stage.",
                );
                assert.eq(explain, sourceSearch["explain"]);
            }
        } else {
            assert(explainStage.hasOwnProperty("explain"), explainStage);
            // We don't know the actual value of the explain object for e2e tests and can't check it.
            if (!isE2E) {
                assert(
                    explain,
                    "Explain is null but needs to be provided for initial mongot stage.",
                );
                assert.eq(explain, explainStage["explain"]);
                if (numFiltered) {
                    assert(explainStage.hasOwnProperty("numDocsFilteredByIdLookup"), explainStage);
                    assert.eq(explainStage["numDocsFilteredByIdLookup"], numFiltered);
                }
            }
        }
    }
}
