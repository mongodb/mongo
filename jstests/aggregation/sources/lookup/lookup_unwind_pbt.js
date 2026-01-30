/**
 * Property-based tests for pipelines that contain a $lookup immediately followed
 * by $unwind on the join array. Verifies correctness against a deoptimized collscan baseline.
 *
 * @tags: [
 *   query_intensive_pbt,
 *   requires_getmore,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {createCorrectnessProperty} from "jstests/libs/property_test_helpers/common_properties.js";
import {getCollectionModel} from "jstests/libs/property_test_helpers/models/collection_models.js";
import {getEqLookupUnwindAggPipelineArb} from "jstests/libs/property_test_helpers/models/query_models.js";
import {makeWorkloadModel} from "jstests/libs/property_test_helpers/models/workload_models.js";
import {testProperty} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {getNestedProperties} from "jstests/libs/query/analyze_plan.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Returning early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

// FCV-based guard to filter out known problematic patterns on older versions.
const is83orAbove = (() => {
    const fcvResult = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}).featureCompatibilityVersion;
    const version = typeof fcvResult === "string" ? fcvResult : fcvResult.version;
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

// Tweak these while iterating; bump them up when the test is stable.
const numRuns = 20;
const numQueriesPerRun = 10;

function makeLookupUnwindAggModel(baseCollectionName, foreignCollectionName) {
    // Allow both self-lookups and lookups to a different collection.
    const foreignCollectionNameArb = fc.constantFrom(baseCollectionName, foreignCollectionName);
    let pipelineArb = getEqLookupUnwindAggPipelineArb(foreignCollectionNameArb, {deterministicBag: true});
    if (!is83orAbove) {
        function hasElemMatch(pipeline) {
            return getNestedProperties(pipeline, "$elemMatch").length > 0;
        }
        pipelineArb = pipelineArb.filter((pipeline) => !hasElemMatch(pipeline));
    }

    function buildAggCommand(pipeline) {
        return {pipeline, options: {}};
    }
    return pipelineArb.map(buildAggCommand);
}

/**
 * Rewrite the pipeline such than $lookup.from points to 'newFrom' instead of the original collection.
 */
function rewriteLookupFrom(pipeline, rewrites) {
    function rewriteStage(stage) {
        if (!stage.hasOwnProperty("$lookup")) {
            return stage;
        }

        const lookup = Object.assign({}, stage["$lookup"]);
        const result = rewrites[lookup.from];
        assert(result, `Unexpected lookup.from value. "${lookup.from}" doesn't match any rewrite rules.`);
        lookup.from = result;
        return {$lookup: lookup};
    }

    return pipeline.map(rewriteStage);
}

describe("$lookup-$unwind", function () {
    it("Optimised $lookup-$unwind queries should have the same results as non-optimised ones", function () {
        const controlColl = db[`${jsTestName()}_control`];
        const foreignControlColl = db[`${jsTestName()}_foreign_control`];
        const experimentColl = db[`${jsTestName()}_experiment`];
        const foreignExperimentColl = db[`${jsTestName()}_foreign_experiment`];
        const aggModel = makeLookupUnwindAggModel(controlColl.getName(), foreignControlColl.getName());
        const correctnessProperty = createCorrectnessProperty(controlColl, experimentColl, {
            modifyExperimentQueryFn: (query) => {
                // Re-write the lookups to point to the experiment collections.
                query.pipeline = rewriteLookupFrom(query.pipeline, {
                    [controlColl.getName()]: experimentColl.getName(),
                    [foreignControlColl.getName()]: foreignExperimentColl.getName(),
                });
                return query;
            },
        });
        testProperty(
            correctnessProperty,
            {controlColl, experimentColl, foreignControlColl, foreignExperimentColl},
            makeWorkloadModel({
                collModel: getCollectionModel(),
                aggModel,
                numQueriesPerRun,
                includeForeignCollection: true,
            }),
            numRuns,
        );
    });
});
