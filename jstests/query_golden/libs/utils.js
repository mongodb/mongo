import {show} from "jstests/libs/golden_test.js";
import {sequentialIds} from "jstests/query_golden/libs/example_data.js";

/**
 * Drops 'coll' and repopulates it with 'docs' and 'indexes'. Sequential _ids are added to
 * documents which do not have _id set.
 */
export function resetCollection(coll, docs, indexes = []) {
    coll.drop();

    const docsWithIds = sequentialIds(docs);
    jsTestLog("Resetting collection. Inserting docs:");
    show(docsWithIds);

    assert.commandWorked(coll.insert(docsWithIds));
    print(`Collection count: ${coll.find().itcount()}`);

    if (indexes.length > 0) {
        jsTestLog("Creating indexes:");
        show(indexes);
        for (let indexSpec of indexes) {
            assert.commandWorked(coll.createIndex(indexSpec));
        }
    }
}

/**
 * Reduces a query plan in-place to a more compact representation by retaining only the fields
 * that pertain to stage names, filtering and index usage. This representation is suitable for
 * golden tests such as plan_stability.js where we want to record the general shape of the
 * query plan on a single line.
 */
export function trimPlanToStagesAndIndexes(obj) {
    const fieldsToKeep = ["stage", "inputStage", "inputStages", "indexName", "indexBounds", "filter"];

    if (typeof obj !== "object" || obj === null) {
        return obj;
    }
    for (let key in obj) {
        if (!Array.isArray(obj) && !fieldsToKeep.includes(key)) {
            delete obj[key];
        } else if (key == "filter") {
            // Preserve the presence of a filter without retaining the actual expression
            obj[key] = true;
        } else {
            if (typeof obj[key] === "object" && obj[key] !== null && key !== "indexBounds") {
                trimPlanToStagesAndIndexes(obj[key]);
            }
        }
    }
    return obj;
}

export function padNumber(num) {
    return num.toString().padStart(6, " ");
}
/**
 * Computes an abstract sort effort for the query, defined as
 * (LOG(nReturned) + 1) * inputStage.nReturned
 */
export function extractSortEffort(stage) {
    let effort = 0;

    if (stage.stage === "SORT") {
        if (stage.inputStage.nReturned > 0) {
            // We +1 here because log(1) = 0 but the effort is still non-zero.
            effort += (Math.log(stage.nReturned) + 1) * stage.inputStage.nReturned;
        }
    }

    if (stage.inputStage) {
        effort += extractSortEffort(stage.inputStage);
    }

    if (stage.inputStages) {
        for (const inputStage of stage.inputStages) {
            effort += extractSortEffort(inputStage);
        }
    }

    return Math.round(effort);
}

export function runPlanStabilityPipelines(db, collName, pipelines) {
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
        // JSON does not allow trailing commas.
        const separator = index === pipelines.length - 1 ? "" : ",";

        // We print the pipeline here so that, even if the test fails,
        // we have already emitted the failing pipeline.
        print(`{">>>pipeline": ${JSON.stringify(pipeline)},`);

        // We do not use explain() as it loses the errmsg in case of an error.
        const explain = db.runCommand({
            explain: {aggregate: collName, pipeline: pipeline, cursor: {}},
            verbosity: "executionStats",
        });

        const executionStats = explain.executionStats;

        if (explain.ok !== 1) {
            let error = "unknown error";
            if (explain.hasOwnProperty("errmsg")) {
                error = explain.errmsg;
            } else if (
                explain.hasOwnProperty("executionStats") &&
                explain.executionStats.hasOwnProperty("errorMessage")
            ) {
                error = explain.executionStats.errorMessage;
            }
            print(`    "error": ${JSON.stringify(error)}}${separator}`);
            totalErrors++;
            return;
        }

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
        print();
    });
    print("],");

    print(
        '">>>totals": {' +
            `"pipelines": ${pipelines.length}, ` +
            `"plans": ${totalPlans}, ` +
            `"keys": ${padNumber(totalKeys)}, ` +
            `"docs": ${padNumber(totalDocs)}, ` +
            `"sorts": ${padNumber(totalSorts)}, ` +
            `"rows": ${padNumber(totalRows)}, ` +
            `"errors": ${padNumber(totalErrors)}},`,
    );

    const parameters = {
        planRankerMode: null,
        samplingMarginOfError: null,
        samplingConfidenceInterval: null,
        internalQuerySamplingCEMethod: null,
        internalQuerySamplingBySequentialScan: null,
    };

    for (const param in parameters) {
        const result = db.adminCommand({getParameter: 1, [param]: 1});
        parameters[param] = result[param];
    }

    print(`">>>parameters": ${JSON.stringify(parameters)}}`);

    jsTest.log.info("See README.plan_stability.md for more information.");
}
