/**
 * Utility functions for explain() results of search + non-search queries.
 */

import {
    getAggPlanStages,
    getLookupStage,
    getUnionWithStage
} from "jstests/libs/query/analyze_plan.js";
import {
    prepareUnionWithExplain,
    validateMongotStageExplainExecutionStats,
    verifyShardsPartExplainOutput
} from "jstests/with_mongot/common_utils.js";

/**
 * This function checks that the explain output contains the view pipeline $_internalSearchIdLookup.
 * This should only be the case when there is a $search stage on the top-level of the user
 * pipeline.
 *
 * @param {Object} explainOutput The explain output from the aggregation.
 * @param {Array} viewPipeline The view pipeline to verify existence of in $_internalSearchIdLookup.
 */
function assertIdLookupContainsViewPipeline(explainOutput, viewPipeline) {
    const stages = getAggPlanStages(explainOutput, "$_internalSearchIdLookup");
    assert(
        stages.length > 0,
        "There should be at least one stage corresponding to $_internalSearchIdLookup in the explain output. " +
            tojson(explainOutput));

    for (let stage of stages) {
        const idLookupFullSubPipe = stage["$_internalSearchIdLookup"]["subPipeline"];
        // The _idLookup subPipeline should be a $match on _id followed by the view stages.
        const idLookupStage = {"$match": {"_id": {"$eq": "_id placeholder"}}};
        assert.eq(idLookupFullSubPipe[0], idLookupStage);
        // Make sure that idLookup subpipeline contains all of the view stages.
        const idLookupViewStages =
            idLookupFullSubPipe.length > 1 ? idLookupFullSubPipe.slice(1) : [];
        assert.eq(idLookupViewStages.length, viewPipeline.length);
        for (let i = 0; i < idLookupViewStages.length; i++) {
            const stageName = Object.keys(viewPipeline[i])[0];
            assert(idLookupViewStages[i].hasOwnProperty(stageName));
        }
    }
}

/**
 * If the top-level aggregation contains a mongot stage, it asserts that the view transforms are
 * contained in _idLookup's subpipeline.  If the top-level aggregation doesn't have a mongot stage,
 * it asserts that the view stages were applied to the beginning of the top-level pipeline.
 *
 * @param {Array} explainOutput The explain output from the aggregation.
 * @param {Array} userPipeline The request/query that was run on the view.
 * @param {Array} viewPipeline The pipeline used to define the view.
 */
export function assertViewAppliedCorrectly(explainOutput, userPipeline, viewPipeline) {
    if (userPipeline.length > 0 &&
        (userPipeline[0].hasOwnProperty("$search") ||
         userPipeline[0].hasOwnProperty("$vectorSearch"))) {
        // The view pipeline is pushed down to a desugared stage, $_internalSearchdLookup. Therefore
        // we inspect the stages (which represent the fully desugared pipeline from the user) to
        // ensure the view was successfully pushed down.
        assertIdLookupContainsViewPipeline(explainOutput, viewPipeline);
    } else {
        // Whereas view transforms for mongot queries happen after desugaring, regular queries apply
        // the view transforms during query expansion. For this reason, we can just inspect the
        // explain's command obj, which represents the expanded query (eg the query after
        // ResolvedView::asExpandedViewAggregation() was called). This also makes it easier to keep
        // explain checks simpler/more consistent between variants that run with SBE turned on and
        // SBE turned off, as SBE greatly changes how the stages are portrayed.
        assert.eq(explainOutput.command.pipeline.slice(0, viewPipeline.length), viewPipeline);
    }
}

/**
 * This helper inspects explain.stages to ensure the view pipeline wasn't applied to the final
 * execution pipeline.
 *
 * @param {Object} explainOutput The explain output from the aggregation.
 * @param {Array} userPipeline The user pipeline ran on the view. This is only used to check if the
 *     pipeline contains a top level $search/$vectorSearch stage.
 * @param {Array} viewPipeline The view pipeline to verify non-existence of in the explain output.
 */
export function assertViewNotApplied(explainOutput, userPipeline, viewPipeline) {
    // Assert that the view pipeline wasn't pushed down to $_internalSearchIdLookup by ensuring
    // there are no $_internalSearchIdLookup stages.
    let stages = getAggPlanStages(explainOutput, "$_internalSearchIdLookup");
    assert(
        stages.length == 0,
        "There should not be any stages corresponding to $_internalSearchIdLookup in the explain output. " +
            tojson(explainOutput));

    // If a view pipeline isn't pushed down to idLookup, there is a risk it was appended to the user
    // pipeline (as is the case for non-search queries on views). It's important to call out that
    // this check investigates the most basic case for non-search queries on views, where the view
    // pipeline isn't desugared and none of the view stages are pushed down or otherwise rearranged
    // during optimization.
    if (userPipeline.length > 0 &&
        (userPipeline[0].hasOwnProperty("$search") ||
         userPipeline[0].hasOwnProperty("$vectorSearch"))) {
        assert.neq(explainOutput.command.pipeline.slice(0, viewPipeline.length), viewPipeline);
    }
}

/**
 * This function is used to assert that the view definition from the search stage *inside* of
 * the $unionWith is applied as intended to $_internalSearchIdLookup.
 *
 * @param {Object} explainOutput The explain output from the whole aggregation.
 * @param {Object} collNss The underlying collection namespace of the view that the $unionWith stage
 *     is run on.
 * @param {Object} viewNss The view namespace that the $unionWith stage is run on.
 * @param {Array} viewPipeline The view pipeline referenced by the search query inside of the
 *     $unionWith.
 * @param {boolean} isStoredSource Whether the $search query is storedSource or not.
 */
export function assertUnionWithSearchSubPipelineAppliedViews(
    explainOutput, collNss, viewNss, viewPipeline, isStoredSource = false) {
    const unionWithStage = getUnionWithStage(explainOutput);
    const unionWithExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);
    if (!isStoredSource) {
        // In fully-sharded environments, check for the correct viewNss on the view object and
        // $unionWith stage. For single node and single shard environments, there is no view object
        // that will exist on the $unionWith explain output. We can still assert that the
        // $unionWith.coll is "resolved" to its collNss. Note that the viewNss is not resolved to
        // its collNss in full-sharded environments, and this is intended behavior.
        if (unionWithExplain.hasOwnProperty("splitPipeline") &&
            unionWithExplain["splitPipeline"] !== null) {
            const firstStage = unionWithExplain.splitPipeline.shardsPart[0];

            // The first stage is either $search or $vectorSearch.
            if (firstStage.hasOwnProperty("$search")) {
                assert.eq(firstStage.$search.view.nss, viewNss);
            } else if (firstStage.hasOwnProperty("$vectorSearch")) {
                assert.eq(firstStage.$vectorSearch.view.nss, viewNss);
            } else {
                assert.fail(
                    "Expected first stage to have either $search or $vectorSearch, but found neither: " +
                    tojson(firstStage));
            }
            assert.eq(unionWithStage.$unionWith.coll, viewNss.getName());
        } else {
            assert.eq(unionWithStage.$unionWith.coll, collNss.getName());
        }

        assertIdLookupContainsViewPipeline(unionWithExplain, viewPipeline);
    } else {
        // Because we are passing through an inner $unionWith explain (i.e. not a normal explain),
        // there is no userPipeline check.
        assertViewNotApplied(unionWithExplain, [], viewPipeline);
    }
}

/**
 * This function is used to assert that the $lookup stage exists as expected in the explain
 * output. Note that $lookup doesn't apply the view definition in explain like $unionWith does.
 *
 * @param {Object} explainOutput The explain output from the whole aggregation.
 * @param {Object} lookupStage The lookup stage to be searched for in the explain output.
 */
export function assertLookupInExplain(explainOutput, lookupStage) {
    // Find the lookup stage in the explain output and assert that it matches the lookup passed
    // to the function.
    const stage = getLookupStage(explainOutput);
    assert(stage,
           "There should be one $lookup stage in the explain output. " + tojson(explainOutput));

    // The explain might add extra stages to the $lookup in its output which is why we can't
    // simply assert that the two BSON objects match each other.
    Object.keys(lookupStage["$lookup"]).forEach((lookupKey) => {
        assert(stage["$lookup"].hasOwnProperty(lookupKey),
               "There should be a key \"" + lookupKey +
                   "\" in the lookup stage from the explain output." + tojson(stage));

        // On the "pipeline" key, simply make sure that the two pipelines have the same length.
        // There are some optimizations in various environment configurations that will make the
        // actual content of the pipeline different, but the length should stay the same.
        if (lookupKey == "pipeline") {
            assert.eq(stage["$lookup"][lookupKey].length,
                      lookupStage["$lookup"][lookupKey].length,
                      "The $lookup stage in the explain output should have the same number of " +
                          "stages as the lookup stage passed to the function." + tojson(stage));
        }
    });
}

/**
 * This function checks that the explain output for $search queries from an e2e test contains
 * the information that it should.
 * @param {Object} explainOutput the results from running coll.explain().aggregate([[$search:
 *     ....],
 *     ...])
 * @param {string} stageType ex. "$_internalSearchMongotRemote" , "$_internalSearchIdLookup "
 * @param {string} verbosity The verbosity of explain. "nReturned" and
 *     "executionTimeMillisEstimate" will not be checked for 'queryPlanner' verbosity "
 * @param {NumberLong} nReturned not needed if verbosity is 'queryPlanner'. For a sharded
 *     scenario, this should be the total returned across all shards.
 */
export function verifyE2ESearchExplainOutput(
    {explainOutput, stageType, verbosity, nReturned = null}) {
    if (explainOutput.hasOwnProperty("splitPipeline") && explainOutput["splitPipeline"] !== null) {
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
 * This function checks that the explain output for $searchMeta queries from an e2e test
 * contains the information that it should.
 * @param {Object} explainOutput the results from running
 *     coll.explain().aggregate([[$searchMeta: ....], ...])
 * @param {NumberLong} numFacetBucketsAndCount The number of documents mongot returns for the
 *     query in the sharded scenario. This should be the number of facet buckets involved in the
 *     query and one more for count.
 * @param {string} verbosity The verbosity of explain. "nReturned" and
 *     "executionTimeMillisEstimate" will not be checked for 'queryPlanner' verbosity "
 */
export function verifyE2ESearchMetaExplainOutput(
    {explainOutput, numFacetBucketsAndCount, verbosity}) {
    // In an unsharded scenario, $searchMeta returns one document with all of the facet values.
    let nReturned = 1;
    if (explainOutput.hasOwnProperty("splitPipeline") && explainOutput["splitPipeline"] !== null) {
        // We check metadata and protocol version for sharded $search.
        verifyShardsPartExplainOutput({result: explainOutput, searchType: "$searchMeta"});
        // In the sharded scenario, $searchMeta returns one document per facet bucket + count,
        // as it needs to be merged in the merging pipeline.
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
 * This function checks that the explain output for $vectorSearch queries from an e2e test
 * contains the information that it should.
 *
 * In the sharded scenario, make sure the collection has enough documents to return the limit for
 * each shard.
 * @param {Object} explainOutput the results from running
 *     coll.explain().aggregate([[$vectorSearch:....], ...])
 * @param {string} stageType ex. "$vectorSearch", $_internalSearchMongotRemote" ,
 * @param {NumberLong} limit The limit for the $vectorSearch query. In a sharded scenario, this
 *     applies per shard.
 * @param {string} verbosity The verbosity of explain. "nReturned" and
 *     "executionTimeMillisEstimate" will not be checked for 'queryPlanner' verbosity "
 */
export function verifyE2EVectorSearchExplainOutput({explainOutput, stageType, limit, verbosity}) {
    let stages = getAggPlanStages(explainOutput, stageType);
    // For $vectorSearch, the limit in the query is applied per shard. This means that nReturned
    // for each shard should match the limit. We don't need to differentiate between the sharded
    // and unsharded scenario.
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
