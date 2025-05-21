/**
 * Utility functions for explain() results of search + non-search queries run on a view.
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

function assertIdLookupContainsViewPipeline(explain, viewPipeline) {
    const stages = getAggPlanStages(explain, "$_internalSearchIdLookup");
    assert(
        stages.length > 0,
        "There should be at least one stage corresponding to $_internalSearchIdLookup in the explain output. " +
            tojson(explain));

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

export function assertToplevelAggContainsView(explainStages, viewPipeline) {
    for (let i = 0; i < viewPipeline.length; i++) {
        let stageName = Object.keys(viewPipeline[i])[0];
        assert(explainStages[i].hasOwnProperty(stageName));
    }
}

/**
 * If the top-level aggregation contains a mongot stage, it asserts that the view transforms are
 * contained in _idLookup's subpipeline.  If the top-level aggregation doesn't have a mongot stage,
 * it asserts that the view stages were applied to the beginning of the top-level pipeline.
 * @param {Array} explainOutput The explain obj containing a list of stages returned from explain().
 * @param {Array} userPipeline The request/query that was run on the view.
 * @param {Object} viewPipeline The pipeline used to define the view.
 */
function assertViewAppliedCorrectlyInExplainStages(explainOutput, userPipeline, viewPipeline) {
    if (userPipeline.length > 0 &&
        (userPipeline[0].hasOwnProperty("$search") ||
         userPipeline[0].hasOwnProperty("$vectorSearch"))) {
        // The view pipeline is pushed down to a desugared stage, $_internalSearchdLookup. Therefore
        // we inspect the stages (which represent the fully desugared pipeline from the user) to
        // ensure the view was successfully pushed down.
        return assertIdLookupContainsViewPipeline(explainOutput, viewPipeline);
    }
    // Whereas view transforms for mongot queries happen after desugaring, regular queries apply the
    // view transforms during query expansion. For this reason, we can just inspect the explain's
    // command obj, which represents the expanded query (eg the query after
    // ResolvedView::asExpandedViewAggregation() was called). This also makes it easier to keep
    // explain checks simpler/more consistent between variants that run with SBE turned on and SBE
    // turned off, as SBE greatly changes how the stages are portrayed.
    return assertToplevelAggContainsView(explainOutput, viewPipeline);
}

/**
 * This helper is intended for inspecting $search and $vectorSearch explain outputs. $searchMeta is
 * excluded because such queries should not invoke view pipelines. The reasoning being that results
 * provide meta data on the enriched collection, they don't have/display the enriched fields
 * themselves. Furthermore, on an implementation level, $searchMeta doesn't desugar to
 * $_internalSearchIdLookup (which performs view transforms for other mongot operators).
 */
export function assertViewAppliedCorrectly(explainOutput, userPipeline, viewPipeline) {
    if (explainOutput.hasOwnProperty("splitPipeline")) {
        // This is a mongot query on a mongot-indexed view over a sharded collection.
        Object.keys(explainOutput.shards).forEach((shardKey) => {
            assertViewAppliedCorrectlyInExplainStages(
                explainOutput.shards[shardKey], userPipeline, viewPipeline);
        });
        return;
    }
    // A mongot query on a mongot-indexed view on non-sharded collection.
    assertViewAppliedCorrectlyInExplainStages(explainOutput, userPipeline, viewPipeline);
}
/**
 * This helper inspects explain.stages to ensure the view pipeline wasn't applied to the final
 * execution pipeline.
 */
export function assertViewNotApplied(explainOutput, viewPipeline) {
    if (explainOutput.hasOwnProperty("splitPipeline")) {
        Object.keys(explainOutput.shards).forEach((shardKey) => {
            /**
             * Assert that the view pipeline wasn't pushed down to $_internalSearchIdLookup by
             * ensuring there is no $_internalSearchIdLookup stage.
             */
            assertViewNotApplied(explainOutput.shards[shardKey], viewPipeline);
        });
        return;
    }
    /**
     * Assert that the view pipeline wasn't pushed down to $_internalSearchIdLookup by ensuring
     * there is no $_internalSearchIdLookup stage.
     */
    let stages = getAggPlanStages(explainOutput, "$_internalSearchIdLookup");
    assert(
        stages.length == 0,
        "There should not be any stages corresponding to $_internalSearchIdLookup in the explain output. " +
            tojson(explainOutput));

    /**
     * If a view pipeline isn't pushed down to idLookup, there is a risk it was appened to the user
     * pipeline (as is the case for non-search queries on views). It's important to call out that
     * this check investigates the most basic case for non-search queries on views, where the view
     * pipeline isn't desugared and none of the view stages are pushed down or otherwise rearranged
     * during optimization.
     */
    if (explainOutput.hasOwnProperty("stages")) {
        assert.neq(explainOutput.stages.slice(0, viewPipeline.length), viewPipeline);
    }
}

export function extractUnionWithSubPipelineExplainOutput(explainStages) {
    for (const stage of explainStages) {
        // Found the $unionWith stage in the explain output.
        if (stage["$unionWith"]) {
            return {"stages": stage["$unionWith"].pipeline};
        }
    }
}
/**
 * As the name attempts to suggest, this functions assumes the user ran a top-level $search
 * aggregation joined via $unionWith with another $search sub-level aggregation. It asserts that the
 * top-level $search explain contains the outerViewPipeline in its _idLookup and similarly, it
 * asserts that the $unionWith $search subpipeline contains the innerView pipeline in its idLookup.
 */
export function assertUnionWithSearchPipelinesApplyViews(
    explain, outerViewPipeline, innerCollName, innerViewName, innerViewPipeline) {
    // This will assert that the top-level search has the view correctly pushed down to idLookup.
    assertIdLookupContainsViewPipeline(explain, outerViewPipeline);

    // Make sure the $unionWith.search subpipeline has the view correctly pushed down to idLookup.
    assertUnionWithSearchSubPipelineAppliedViews(
        explain, innerCollName, innerViewName, innerViewPipeline);
}

/**
 * This function is used to assert that the view definition from the search stage *inside* of
 * the $unionWith is applied as intended to $_internalSearchIdLookup.
 *
 * @param {*} explain The explain output from the whole aggregation.
 * @param {*} viewPipeline The view pipeline referenced by the search query inside of the
 *     $unionWith.
 */
export function assertUnionWithSearchSubPipelineAppliedViews(
    explain, collNss, viewNss, viewPipeline) {
    const unionWithStage = getUnionWithStage(explain);
    const unionWithExplain = prepareUnionWithExplain(unionWithStage.$unionWith.pipeline);

    // In fully-sharded environments, check for the correct viewNss on the view object and
    // $unionWith stage. For single node and single shard environments, there is no view object that
    // will exist on the $unionWith explain output. We can still assert that the $unionWith.coll is
    // "resolved" to its collNss. Note that the viewNss is not resolved to its collNss in
    // full-sharded environments, and this is intended behavior.
    if (unionWithExplain.hasOwnProperty("splitPipeline") &&
        unionWithExplain["splitPipeline"] !== null) {
        assert.eq(unionWithExplain.splitPipeline.shardsPart[0].$search.view.nss, viewNss);
        assert.eq(unionWithStage.$unionWith.coll, viewNss.getName());
    } else {
        assert.eq(unionWithStage.$unionWith.coll, collNss.getName());
    }

    assertIdLookupContainsViewPipeline(unionWithExplain, viewPipeline);
}

/**
 * This function is used to assert that the $lookup stage exists as expected in the explain output.
 * Note that $lookup doesn't apply the view definition in explain like $unionWith does.
 *
 * @param {*} explain The explain output from the whole aggregation.
 * @param {*} lookupStage The lookup stage to be searched for in the explain output.
 */
export function assertLookupInExplain(explain, lookupStage) {
    // Find the lookup stage in the explain output and assert that it matches the lookup passed
    // to the function.
    const stage = getLookupStage(explain);
    assert(stage, "There should be one $lookup stage in the explain output. " + tojson(explain));

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
 * This function assumes that the user ran a search stage inside of a $lookup pipeline and the
 * $lookup is ran on a view. Given the explain output from such an aggregation, we must make sure
 * that the view is applied before the $lookup and that the $lookup contains the search stage
 * specified.
 */
export function assertLookupWithSearchPipelineAppliedViews(
    explain, lookupStage, viewPipeline, isSbeEnabled = false) {
    // $lookup does not include explain info about its subpipeline, so when there is a search
    // stage inside of the lookup's pipeline we don't expect to see the desugared stages in the
    // explain output.
    if (isSbeEnabled) {
        assertViewAppliedCorrectly(explain.command.pipeline, lookupStage, viewPipeline);
    } else {
        // Make sure that the view definition is applied before $lookup. The stages array always
        // begins with $cursor so we must shift the array before asserting the view.
        if (explain.hasOwnProperty("shards")) {
            Object.keys(explain.shards).forEach((shardKey) => {
                explain.shards[shardKey].stages.shift();
                assertToplevelAggContainsView(explain.shards[shardKey].stages, viewPipeline);
            });
        } else {
            explain.stages.shift();
            assertToplevelAggContainsView(explain.stages, viewPipeline);
        }
    }

    assertLookupInExplain(explain, lookupStage);
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
