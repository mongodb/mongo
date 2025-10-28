/**
 * A property-based test that asserts the correctness of collated queries. Like
 * collation_correctness_pbt.js, but ensures that the collation is fixed for queries and indexes
 * within a single iteration.
 *
 * @tags: [
 * query_intensive_pbt,
 * # Runs queries that may return many results, requiring getmores.
 * requires_getmore,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * # Multiversion tests rediscover an issue where a buggy optimization omits $sortKey when required.
 * requires_fcv_83
 * ]
 */

import {getQueryAndOptionsModel} from "jstests/libs/property_test_helpers/models/query_models.js";
import {collationArb} from "jstests/libs/property_test_helpers/models/collation_models.js";
import {createCacheCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getIndexModel, getTimeSeriesIndexModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getMatchArb} from "jstests/libs/property_test_helpers/models/match_models.js";
import {
    simpleProjectArb,
    addFieldsConstArb,
    computedProjectArb,
    addFieldsVarArb,
    getSortArb,
} from "jstests/libs/property_test_helpers/models/query_models.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getDatasetModel} from "jstests/libs/property_test_helpers/models/document_models.js";

if (isSlowBuild(db)) {
    jsTestLog("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 20;
const numQueriesPerRun = 40;

const controlColl = db.fixed_collation_pbt_control;
const experimentColl = db.fixed_collation_pbt_experiment;

// This property compares query results where the control has optimizations, indexed plans, and the
// plan cache disabled.
const correctnessProperty = createCacheCorrectnessProperty(controlColl, experimentColl);

// Note that we exclude $group here on purpose. With collation, different strings will compare as
// equal, but the output of the $group stage depends on which one is seen first. This is not
// guaranteed to be the same across the control/experiment collections.
const allowedStages = [
    simpleProjectArb,
    getMatchArb(),
    addFieldsConstArb,
    computedProjectArb,
    addFieldsVarArb,
    getSortArb(),
];
// Generate queries without collation and then we will append it later.
const aggModel = getQueryAndOptionsModel({allowCollation: false, allowedStages: allowedStages});

function makeCollationWorkloadModel(isTS) {
    // Generate indexes without collation and then we will append it later.
    const indexArb = isTS ? getTimeSeriesIndexModel({allowCollation: false}) : getIndexModel({allowCollation: false});

    return fc
        .record({
            // This collation will be used for all queries and indexes in a single round so that every
            // query is eligible to use the indexes.
            sharedCollation: collationArb,
            docs: getDatasetModel(),
            indexes: fc.array(indexArb, {minLength: 0, maxLength: 15, size: "+2"}),
            queries: fc.array(aggModel, {minLength: 1, maxLength: numQueriesPerRun, size: "+2"}),
        })
        .map(({sharedCollation, docs, indexes, queries}) => {
            const indexesWithCollation = indexes.map(({def, options}) => {
                return {
                    def,
                    options: Object.assign({}, options, {collation: sharedCollation}),
                };
            });

            const queriesWithCollation = queries.map((query) => ({
                "pipeline": query.pipeline,
                "options": Object.assign({}, query.options, {collation: sharedCollation}),
            }));
            return {collSpec: {isTS: isTS, docs, indexes: indexesWithCollation}, queries: queriesWithCollation};
        });
}

testProperty(correctnessProperty, {controlColl, experimentColl}, makeCollationWorkloadModel(false), numRuns);

// TODO SERVER-103381 implement time-series PBT testing.
// testProperty(correctnessProperty, {controlColl, experimentColl}, makeCollationWorkloadModel(true), numRuns);
