/**
 * When featureFlagGetExecutorDeferredEngineChoice is enabled, queries that use SBE and queries
 * that use classic may share a cache entry. This is a property-based test that targets shared
 * cache entries, by running queries that will use classic and similar queries that use SBE.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_after_drop,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * does_not_support_transactions,
 * # Plan cache key may change in between versions.
 * multiversion_incompatible
 * ]
 */
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {
    testProperty,
    concreteQueryFromFamily,
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {findLayerOnlyPipeline} from "jstests/libs/property_test_helpers/common_models.js";
import {getIndexesModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {getPlanCacheKeyFromPipeline} from "jstests/libs/query/analyze_plan.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

const is83orAbove = isFCVgte(db, "8.3");

const numRuns = 15;
const numQueriesPerRun = 25;

const controlColl = db.cache_correctness_switch_engines_pbt_control;
const experimentColl = db.cache_correctness_switch_engines_pbt_experiment;

const group = {$group: {_id: "$a", acc: {$max: "$b"}}};
const projectRequiredFields = {$project: {_id: 0, a: 1, b: 1}};
{
    // Assert that these two pipelines share a cache key.
    const tempColl = db.temp;
    tempColl.drop();
    assert.commandWorked(tempColl.insert({a: 1, b: 1}));
    assert.eq(
        getPlanCacheKeyFromPipeline([projectRequiredFields], tempColl),
        getPlanCacheKeyFromPipeline([group], tempColl),
        "expected classic + $project and SBE + $group queries to share a plan cache key",
    );
}

/*
 * We'll construct a model that generates a query that will use classic:
 *     [find-layer-stage+, $project]
 * And a query that will use SBE, with the same find prefix shape (but could be different constants):
 *     [find-layer-stage+, $group]
 * The $project in the classic query is needed to match the cache key for the SBE query, since the $group
 * will indicate which fields are needed to the find layer, and project all other fields out. Without it,
 * they will use different cache entries.
 */

const classicAndSbeQueryPairArb = fc
    .record({
        prefixFamily: findLayerOnlyPipeline({is83orAbove}),
        runClassicFirst: fc.boolean(),
    })
    .map(({prefixFamily, runClassicFirst}) => {
        // Query models produce "families" of stages, where several choices for constants exist
        // at the leaves. We can extract the first two sets of constants to use for the classic
        // and SBE queries. They may happen to be the same constants, but usually they'll be different.
        const classicPrefix = concreteQueryFromFamily(prefixFamily, 0);
        const sbePrefix = concreteQueryFromFamily(prefixFamily, 1);

        const classicQuery = [...classicPrefix, projectRequiredFields];
        const sbeQuery = [...sbePrefix, group];
        return {classicQuery, sbeQuery, runClassicFirst};
    });

const queriesModel = fc
    .array(classicAndSbeQueryPairArb, {minLength: 1, maxLength: numQueriesPerRun})
    .map((queryPairs) => {
        const queries = [];
        for (const {classicQuery, sbeQuery, runClassicFirst} of queryPairs) {
            // We'd like one engine to be used to cache the plan, and a different engine to be
            // used when fetching the plan. Depending on which engine should cache, we place
            // three of the same query first, then one query for the opposite engine after.
            // The first three will create the cache entry, the fourth query will use it with
            // the opposite engine
            if (runClassicFirst) {
                queries.push(classicQuery, classicQuery, classicQuery, sbeQuery);
            } else {
                queries.push(sbeQuery, sbeQuery, sbeQuery, classicQuery);
            }
        }
        return queries.map((q) => ({pipeline: q, options: {}}));
    });

const workloadModel = fc.record({
    // Increase the number of indexes so the cache is used more often.
    collSpec: getCollectionModel({
        indexesModel: getIndexesModel({minNumIndexes: 20, maxNumIndexes: 20}),
    }),
    queries: queriesModel,
});

testProperty(
    createCorrectnessProperty(controlColl, experimentColl),
    {controlColl, experimentColl},
    workloadModel,
    numRuns,
);
