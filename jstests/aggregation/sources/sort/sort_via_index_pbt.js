/**
 * A property-based test that targets queries using an index scan to satisfy a $sort. It asserts the
 * correctness of these queries.
 *
 * We also have a check to make sure the fast-check model targets this feature effectively. At least
 * 80% of queries should use an index to satisfy a $sort. We can't bring this number to 100% without
 * sacrificing coverage, so instead we aim for a high percentage of winning plans to use the feature
 * The most common case where we don't use the index is:
 *    [{$sort: {a: 1}}, {$sort: {b: 1}}]
 * We generate the $sort on `a` and an index {a: 1}, expecting the $sort to use the index. However
 * our query optimizer realizes that the sort on `b` allows us to remove the sort on `a`.
 * We could prevent the rest of the pipeline from using a $sort, but there are valuable cases that
 * include sorting later, so we keep it.
 * Another case that makes it impossible to reach 100% targeting is because the optimizer may not
 * pick the plan we are looking for. There may be a better plan available.
 *
 * @tags: [
 * query_intensive_pbt,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    getAggPipelineModel,
    getSortArb
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 40;

const controlColl = db.sort_via_index_pbt_control;
const experimentColl = db.sort_via_index_pbt_experiment;

/*
 * Collect statistics on how many plans use the expected index to sort the documents. This helps us
 * make sure our test is targeting the feature effectively.
 */
let totalNumPlans = 0;
let numPlansUsedIndex = 0;
function statsCollectorFn(explain) {
    totalNumPlans++;
    const ixscanStages = getPlanStages(getWinningPlanFromExplain(explain), 'IXSCAN');
    // We place the sort index first, so it will be given the name 'index_0'.
    if (ixscanStages.every(stage => stage.indexName === 'index_0')) {
        numPlansUsedIndex++;
    }
}
const correctnessProperty =
    createCorrectnessProperty(controlColl, experimentColl, statsCollectorFn);

/*
 * Generate a random $sort, aggregation pipelines, and collection. Using the $sort, create an
 * additional index that has the same specifications. Also prefix each aggregation query with the
 * $sort.
 */
function getWorkloadModel(isTS) {
    return fc
        .record({
            sort: getSortArb(8 /* maxNumSortComponents */),
            pipelines: fc.array(getAggPipelineModel(),
                                {minLength: 0, maxLength: numQueriesPerRun, size: '+2'}),
            collSpec: getCollectionModel(isTS)
        })
        .map(({sort, pipelines, collSpec}) => {
            // Prefix every pipeline with the sort operation.
            const pipelinesWithSort = pipelines.map(pipeline => [sort, ...pipeline]);
            // Create an index that will satisfy this sort.
            // TODO SERVER-105223 use other kinds of indexes to satisfy the sort (hashed, wildcard).
            // The server won't let us create an index with pattern {_id: -1}. If we see a sort
            // with only _id, it's not necessary to create an index anyway since we always
            // have {_id: 1}.
            let indexes;
            if (Object.keys(sort.$sort).length === 1 && sort.$sort._id) {
                indexes = [...collSpec.indexes];
            } else {
                indexes = [{def: sort.$sort, options: {}}, ...collSpec.indexes];
            }
            return {collSpec: {isTS, docs: collSpec.docs, indexes}, queries: pipelinesWithSort};
        });
}

testProperty(correctnessProperty,
             {controlColl, experimentColl},
             getWorkloadModel(false /* isTS */),
             numRuns);

// Assert that the number of plans that used the index for the sort is >= 80%
assert.gt(totalNumPlans, 0);
assert.gte(numPlansUsedIndex / totalNumPlans, 0.8, {numPlansUsedIndex, totalNumPlans});

// TODO SERVER-103381 implement time-series PBT testing.
