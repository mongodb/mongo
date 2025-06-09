/**
 * A property-based test that asserts the correctness of queries that begin with $addFields.
 *
 * @tags: [
 * query_intensive_pbt,
 * requires_timeseries,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * ]
 */

import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    addFieldsConstArb,
    addFieldsVarArb,
    getAggPipelineModel
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 40;
const numQueriesPerRun = 40;

const controlColl = db.add_fields_pbt_control;
const experimentColl = db.add_fields_pbt_experiment;

const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl);

const addFieldsArb = fc.oneof(addFieldsConstArb, addFieldsVarArb);
const aggModel = fc.record({addFieldsStage: addFieldsArb, restOfPipeline: getAggPipelineModel()})
                     .map(({addFieldsStage, restOfPipeline}) => {
                         return [addFieldsStage, ...restOfPipeline];
                     });

testProperty(correctnessProperty,
             {controlColl, experimentColl},
             makeWorkloadModel({collModel: getCollectionModel(), aggModel, numQueriesPerRun}),
             numRuns);

// TODO SERVER-103381 implement time-series PBT testing.
