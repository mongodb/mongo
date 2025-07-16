/**
 * Tests that the plans remain stable across releases by comparing the expected plans against the
 * current ones.
 *
 * @tags: [
 * incompatible_aubsan,
 * tsan_incompatible,
 * ]
 *
 */

import {
    checkSbeFullyEnabled,
} from "jstests/libs/query/sbe_util.js";
import {
    extractSortEffort,
    padNumber,
    trimPlanToStagesAndIndexes
} from 'jstests/query_golden/libs/utils.js';
import {pipelines} from 'jstests/query_golden/test_inputs/plan_stability_pipelines.js';
import {
    populateSimplePlanStabilityDataset
} from "jstests/query_golden/test_inputs/simple_plan_stability_dataset.js";

if (checkSbeFullyEnabled(db)) {
    jsTestLog("Skipping the test because CBR only applies to the classic engine.");
    quit();
}

if (db.getServerBuildInfo().isAddressSanitizerActive() ||
    db.getServerBuildInfo().isLeakSanitizerActive() ||
    db.getServerBuildInfo().isThreadSanitizerActive() ||
    db.getServerBuildInfo().isUndefinedBehaviorSanitizerActive()) {
    jsTestLog("Skipping the test because a sanitizer is active.");
    quit();
}

const collName = "plan_stability";

/**
 * We use a dataset with 100K rows so that:
 * 1. Queries still complete in a reasonable time.
 * 2. There will be sufficient difference between plans,
 *    rather than simple "off-by-one" counter increments/decrements.
 */
const collSize = 100_000;

populateSimplePlanStabilityDataset(collName, collSize);

let totalPlans = 0;
let totalKeys = 0;
let totalDocs = 0;
let totalSorts = 0;
let totalRows = 0;
let totalErrors = 0;

/**
 * The output of this test is a JSON that contains both the plans and stats for each pipeline
 * as well as a summary section with totals. The structure of the output is as follows:
 * {
 *     "pipelines: [
 *         {
 *             ">>>pipeline"    : <pipeline>,
 *                 "winningPlan": <winningPlan>,
 *                 "keys"       : <totalKeysExamined>,
 *                 "docs"       : <totalDocsExamined>,
 *                 "sorts"      : <sortEffort>,
 *                 "plans"      : <numberOfPlans>,
 *                 "rows"       : <nReturned>
 *         },
 *         ...
 *     ],
 *     ">>>totals": {
 *         "keys": <totalKeysExamined>, "docs": <totalDocsExamined>, "sortEffort":
 * <totalSortEffort>, "plans": <numberOfPlans>, "rows": <nReturned>
 *     }
 * }
 *
 * The sortEffort is an abstract measure of the complexity of any SORT stages, and is
 * defined as (LOG(nReturned) + 1) * inputStage.nReturned.
 */

print('{">>>pipelines":[');
pipelines.forEach((pipeline, index) => {
    print();
    print(`{">>>pipeline": ${JSON.stringify(pipeline)},`);

    // JSON does not allow trailing commas.
    const separator = index === pipelines.length - 1 ? "" : ",";

    // We do not use explain() as it loses the errmsg in case of an error.
    const explain = db.runCommand({
        explain: {aggregate: collName, pipeline: pipeline, cursor: {}},
        verbosity: "executionStats"
    });

    if (explain.ok !== 1) {
        totalErrors++;
        print(`    "error": ${JSON.stringify(explain.errmsg)}}${separator}`);
        return;
    }

    const executionStats = explain.executionStats;
    const winningPlan = trimPlanToStagesAndIndexes(explain.queryPlanner.winningPlan);

    const plans = explain.queryPlanner.rejectedPlans.length + 1;
    totalPlans += plans;

    const keys = executionStats.totalKeysExamined;
    totalKeys += keys;

    const docs = executionStats.totalDocsExamined;
    totalDocs += docs;

    const nReturned = executionStats.nReturned;
    totalRows += nReturned;

    const sorts = extractSortEffort(executionStats.executionStages);
    totalSorts += sorts;

    print(`    "winningPlan": ${JSON.stringify(winningPlan)},`);
    print(`    "keys" : ${padNumber(keys)},`);
    print(`    "docs" : ${padNumber(docs)},`);
    print(`    "sorts": ${padNumber(sorts)},`);
    print(`    "plans": ${padNumber(plans)},`);
    print(`    "rows" : ${padNumber(nReturned)}}${separator}`);
});
print("],");

print('">>>totals": {' +
      `"plans": ${padNumber(totalPlans)}, ` +
      `"keys": ${padNumber(totalKeys)}, ` +
      `"docs": ${padNumber(totalDocs)}, ` +
      `"sorts": ${padNumber(totalSorts)}, ` +
      `"rows": ${padNumber(totalRows)}, ` +
      `"errors": ${padNumber(totalErrors)}},`);

const parameters = {
    planRankerMode: null,
    samplingMarginOfError: null,
    samplingConfidenceInterval: null,
    internalQuerySamplingCEMethod: null
};

for (const param in parameters) {
    const result = db.adminCommand({getParameter: 1, [param]: 1});
    parameters[param] = result[param];
}

print(`">>>parameters": ${JSON.stringify(parameters)}}`);
