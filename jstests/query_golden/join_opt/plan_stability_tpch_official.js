/**
 * Tests that the TPC-H join plans and results remain stable across releases by comparing the
 * actual plans and results against the expected ones.
 *
 * See README.plan_stability.join_opt.md for more information.
 *
 * @tags: [
 * incompatible_aubsan,
 * tsan_incompatible,
 * ]
 *
 */
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {commands} from "jstests/query_golden/test_inputs/plan_stability_pipelines_tpch_official.js";
import {populateTPCHDataset} from "jstests/libs/query/tpch_dataset.js";
import {isSlowBuild, isRunAllFeatureFlagTests} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {runPlanStabilityCommands, ResultsetRepresentation} from "jstests/query_golden/libs/plan_stability_utils.js";

if (!checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping the test because Join Optimization only applies to SBE.");
    quit();
}

if (isSlowBuild(db)) {
    jsTest.log.info("Skipping the test because a sanitizer is active.");
    quit();
}

assert(
    isRunAllFeatureFlagTests(),
    "This test should be run with --runAllFeatureFlagTests in order to match how evergreen runs it.",
);

jsTest.log.info("See README.plan_stability.join_opt.md for more information.");

const tpch = populateTPCHDataset("0.1");
runPlanStabilityCommands(tpch, commands, ResultsetRepresentation.FULL);

jsTest.log.info("See README.plan_stability.join_opt.md for more information.");
