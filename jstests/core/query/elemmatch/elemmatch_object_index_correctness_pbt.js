/**
 * Checks that an object $elemMatch query returns the same documents whether it uses an index or a
 * collection scan, and that these queries can still use an index at all.
 *
 * @tags: [
 * uses_explain,
 * query_intensive_pbt,
 * does_not_support_transactions,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Time series collections do not support indexing array values in measurement fields.
 * exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {getDifferentlyShapedQueries} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    elemMatchPredicateArb,
    getElemMatchDocsModel,
    getElemMatchIndexesModel,
    nonArrayFieldPredicateArb,
} from "jstests/libs/property_test_helpers/models/elemmatch_models.js";
import {oneof} from "jstests/libs/property_test_helpers/models/model_utils.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const numRuns = 50;
const numQueriesPerRun = 20;

// Measured as numQueriesWithIndexedPlan / numQueries.
// Current master: 0.70, SERVER-125555 reverted: 0.55.
// The constant is exactly in between to catch SERVER-125555.
const minIndexedPlanRatio = 0.62;

const coll = db.elemmatch_object_index_correctness_pbt;

const matchArb = oneof(
    {arbitrary: elemMatchPredicateArb.map((pred) => ({$match: pred})), weight: 4},
    {
        // A predicate outside the array.
        arbitrary: fc
            .tuple(nonArrayFieldPredicateArb, elemMatchPredicateArb)
            .map(([outer, elemMatch]) => ({$match: {$and: [outer, elemMatch]}})),
        weight: 4,
    },
    {
        arbitrary: fc
            .tuple(nonArrayFieldPredicateArb, elemMatchPredicateArb)
            .map(([outer, elemMatch]) => ({$match: {$or: [outer, elemMatch]}})),
        weight: 1,
    },
    {arbitrary: elemMatchPredicateArb.map((pred) => ({$match: {$nor: [pred]}})), weight: 1},
);

const aggModel = fc.record({
    pipeline: fc.array(matchArb, {minLength: 1, maxLength: 2}),
    options: fc.constant({}),
});

let numQueries = 0;
let numQueriesWithIndexedPlan = 0;

function hasIndexedPlan(explain) {
    return JSON.stringify(explain).includes('"IXSCAN"');
}

function elemMatchResultsMatchNaturalScan(getQuery, testHelpers) {
    const queries = getDifferentlyShapedQueries(getQuery, testHelpers);

    for (const {pipeline, options} of queries) {
        const naturalResults = coll
            .aggregate(pipeline, {...options, hint: {$natural: 1}})
            .toArray();
        const optimizedResults = coll.aggregate(pipeline, options).toArray();
        const explain = coll.explain().aggregate(pipeline, options);

        numQueries++;
        if (hasIndexedPlan(explain)) {
            numQueriesWithIndexedPlan++;
        }

        if (!testHelpers.comp(naturalResults, optimizedResults)) {
            return {
                passed: false,
                message:
                    "Query results using an index did not match the results of the same query " +
                    "hinted with {$natural: 1}.",
                query: {pipeline, options},
                explain,
                naturalResults,
                optimizedResults,
            };
        }
    }
    return {passed: true};
}

testProperty(
    elemMatchResultsMatchNaturalScan,
    {experimentColl: coll},
    makeWorkloadModel({
        collModel: getCollectionModel({
            docsModel: getElemMatchDocsModel(),
            indexesModel: getElemMatchIndexesModel(),
        }),
        aggModel,
        numQueriesPerRun,
    }),
    numRuns,
);

jsTest.log.info("$elemMatch index coverage", {
    numQueriesWithIndexedPlan,
    numQueries,
});
assert.gt(numQueries, 0, "no $elemMatch query was generated");
// Checked here rather than with a requires_fcv_83 tag, because that tag would also stop this test
// from running in the FCV upgrade and downgrade passthroughs.
if (isFCVgte(db, "8.3")) {
    assert.gte(
        numQueriesWithIndexedPlan / numQueries,
        minIndexedPlanRatio,
        "too few $elemMatch queries had an index plan available",
        {numQueriesWithIndexedPlan, numQueries, minIndexedPlanRatio},
    );
}
