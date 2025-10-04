/**
 * Tests that the plans remain stable across releases by comparing the expected plans against the
 * current ones. A product of SPM-3816. See README.plan_stability.md for more information.
 *
 * This test uses a simplistic dataset with a few columns, few indexes and trivial data distributions.
 *
 * The queries used in this test are generated using jstestfuzz, using the following grammar:
 * `jstestfuzz:src/fuzzers/plan_stability/plan_stability.ne`
 * before being processed by the scripts in the `feature-extractor` repository at:
 * `feature-extractor:scripts/cbr/`

*
 * @tags: [
 * incompatible_aubsan,
 * tsan_incompatible,
 * ]
 *
 */
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {pipelines} from "jstests/query_golden/test_inputs/plan_stability_pipelines.js";
import {populateSimplePlanStabilityDataset} from "jstests/query_golden/test_inputs/simple_plan_stability_dataset.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {runPlanStabilityPipelines} from "jstests/query_golden/libs/utils.js";

if (checkSbeFullyEnabled(db)) {
    jsTest.log.info("Skipping the test because CBR only applies to the classic engine.");
    quit();
}

if (isSlowBuild(db)) {
    jsTest.log.info("Skipping the test because a sanitizer is active.");
    quit();
}

/**
 * We use a dataset with 100K rows so that:
 * 1. Queries still complete in a reasonable time.
 * 2. There will be sufficient difference between plans,
 *    rather than simple "off-by-one" counter increments/decrements.
 */
const collSize = 100_000;
const collName = jsTestName();

jsTest.log.info("See README.plan_stability.md for more information.");

populateSimplePlanStabilityDataset(collName, collSize);
runPlanStabilityPipelines(db, collName, pipelines);
