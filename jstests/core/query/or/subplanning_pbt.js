/**
 * Property-based test that asserts correctness of queries that begin with a $match with a top-level
 * $or predicate. This tests subplanning code paths which are significantly different from others.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * requires_timeseries,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * ]
 */
import {isSlowBuild} from "jstests/libs/aggregation_pipeline_utils.js";
import {
    createCacheCorrectnessProperty
} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getMatchPredicateSpec} from "jstests/libs/property_test_helpers/models/match_models.js";
import {getAggPipelineModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog('Exiting early because debug is on, opt is off, or a sanitizer is enabled.');
    quit();
}

const numRuns = 15;
const numQueriesPerRun = 20;

const controlColl = db.subplanning_pbt_control;
const experimentColl = db.subplanning_pbt_experiment;
// Use the cache correctness property, which runs similar query shapes with different constant
// values plugged in. We do this because subplanning can have unique interactions with the plan
// cache.
const correctnessProperty = createCacheCorrectnessProperty(controlColl, experimentColl);

// {$match: {$or: ...}}
const matchWithTopLevelOrArb = getMatchPredicateSpec()
                                   .singleCompoundPredicate
                                   .filter(pred => {
                                       // This filter will pass 1/3rd of the time. Since generating
                                       // queries is quick, this isn't a concern.
                                       return Object.keys(pred).includes('$or');
                                   })
                                   .map(pred => {
                                       return {$match: pred};
                                   });
const aggModel = fc.record({orMatch: matchWithTopLevelOrArb, pipeline: getAggPipelineModel()})
                     .map(({orMatch, pipeline}) => {
                         return [orMatch, ...pipeline];
                     });

// Test with a regular collection.
testProperty(correctnessProperty,
             {controlColl, experimentColl},
             makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
             numRuns);

// // TODO SERVER-103381 re-enable PBT testing for time-series
// // Test with a TS collection.
// TODO SERVER-83072 re-enable $group in this test, by removing the filter below.
// const tsAggModel = aggModel.filter(query => {
//     for (const stage of query) {
//         if (Object.keys(stage).includes('$group')) {
//             return false;
//         }
//     }
//     return true;
// });
// testProperty(correctnessProperty,
//              {controlColl, experimentColl},
//              makeWorkloadModel({collModel: getCollectionModel(), aggModel: tsAggModel,
//              numQueriesPerRun}), numRuns);