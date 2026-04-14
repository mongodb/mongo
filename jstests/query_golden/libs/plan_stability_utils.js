import {getPlanRankerMode} from "jstests/libs/query/cbr_utils.js";
import {
    joinPlanToString,
    jsonifyMultilineString,
    newlineBeforeEachStage,
    trimPlanToStagesAndIndexes,
} from "jstests/query_golden/libs/pretty_printers.js";
import {resultsetChecksum} from "jstests/query_golden/libs/checksum_utils.js";

export const ResultsetRepresentation = Object.freeze({
    ROW_COUNT: 1 << 0,
    FULL: 1 << 1,
    CHECKSUM: 1 << 2,
});

export function padNumber(num, width = 6) {
    return num.toString().padStart(width, " ");
}

/**
 * Sort an array (such as a resultset) based on the JSON.stringify() representation of its elements.
 */
export function sortJsonStringify(arr) {
    return arr.toSorted((a, b) => JSON.stringify(a).localeCompare(JSON.stringify(b)));
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

/**
 *  Sum all top-level values for `counter` in the explain.
 *  The drill-down logic is such that we will not take any counters from nested plan stages
 *  into account.
 */
function sumCounters(tree, counter) {
    let sum = 0;

    function walk(node) {
        if (Array.isArray(node)) {
            for (const item of node) {
                walk(item);
            }
            return;
        }

        if (node && typeof node === "object") {
            for (const [key, value] of Object.entries(node)) {
                if (key === "executionStats") {
                    sum += value[counter];
                } else if (key === "$lookup" && node[counter] !== undefined) {
                    // The counter is at the same nesting level as the $lookup
                    sum += node[counter].toNumber();
                }

                // Recurse into child values
                walk(value);
            }
        }
    }

    walk(tree);
    return sum;
}

/**
 * Return the total number of documents returned by checking the nReturned value of the last plan stage
 *
 */
function getNReturned(explain) {
    const lastStage = explain.stages !== undefined ? explain.stages[explain.stages.length - 1] : explain;
    let nReturned;

    if (lastStage.executionStats !== undefined) {
        nReturned = lastStage.executionStats.nReturned;
    } else if (lastStage["$cursor"] !== undefined) {
        nReturned = lastStage["$cursor"].executionStats.nReturned;
    } else if (lastStage.nReturned !== undefined) {
        nReturned = lastStage.nReturned.toNumber();
    }
    assert.neq(nReturned, undefined, `Unable to calculate nReturned from ${tojson(explain)}`);
    return nReturned;
}

/**
 * Produce the output for plan stability golden tests that target CBR
 */
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
    let paramsToRestore;

    // All-feature-flags variants enable CBR even for the
    // query_golden_classic suite.
    // Plan stability test running with CBR need the following
    // knobs. Set them before starting the tests & restore them
    // after, as the query_golden_classic suite runs other
    // golden tests, which do not expect these knobs.
    if (getPlanRankerMode(db) !== "multiPlanning") {
        // CBR enabled
        paramsToRestore = assert.commandWorked(
            db.adminCommand({
                getParameter: 1,
                internalQueryPlannerEnableSortIndexIntersection: 1,
                internalQuerySamplingBySequentialScan: 1,
            }),
        );
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlannerEnableSortIndexIntersection: true,
                internalQuerySamplingBySequentialScan: true,
            }),
        );
    }

    try {
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
            featureFlagCostBasedRanker: null,
            internalQueryCBRCEMode: null,
            samplingMarginOfError: null,
            samplingConfidenceInterval: null,
            internalQuerySamplingCEMethod: null,
            internalQuerySamplingBySequentialScan: null,
        };

        for (const param in parameters) {
            const result = db.adminCommand({getParameter: 1, [param]: 1});
            parameters[param] = result[param];
        }

        if (!(parameters["featureFlagCostBasedRanker"] ?? {})["value"]) {
            // internalQueryCBRCEMode does not matter unless
            // CBR is enabled, and is likely to confuse the
            // reader.
            delete parameters["internalQueryCBRCEMode"];
        } else if (parameters["internalQueryCBRCEMode"] === "automaticCE") {
            const param = "automaticCEPlanRankingStrategy";
            const result = db.adminCommand({getParameter: 1, [param]: 1});
            parameters[param] = result[param];
        }

        // Strip the FCV version from any server parameters that have it.
        for (const [param, value] of Object.entries(parameters)) {
            if (value && typeof value === "object" && "version" in value) {
                delete parameters[param].version;
            }
        }

        print(`">>>parameters": ${JSON.stringify(parameters)}}`);

        jsTest.log.info("See README.plan_stability.md for more information.");
    } finally {
        if (paramsToRestore) {
            // Restore the parameters we changed
            assert.commandWorked(
                db.adminCommand(
                    Object.fromEntries([
                        ["setParameter", 1],
                        ...Object.entries(paramsToRestore)
                            .filter(([k, _]) => k !== "ok" && k !== "operationTime" && k !== "$clusterTime")
                            .map(([param, value]) => [param, typeof value === "string" ? value : value["value"]]),
                    ]),
                ),
            );
        }
    }
}

/**
 * Produce the output for plan stability golden tests that target Join Optimization
 */

export function runPlanStabilityCommands(
    db,
    commands,
    resultsetRepresentation = ResultsetRepresentation.ROW_COUNT | ResultsetRepresentation.CHECKSUM,
) {
    let totalKeys = 0;
    let totalDocs = 0;
    let totalRows = 0;

    /**
     * The output of this test is a JSON that contains both the plans and stats for each command
     * as well as a summary section with totals. The structure of the output is as follows:
     * {
     *     ">>>commands": [
     *         {
     *             ">>>command"    : <command>,
     *                 "winningPlan": <winningPlan>,
     *                 "keys"       : <totalKeysExamined>,
     *                 "docs"       : <totalDocsExamined>,
     *                 "rows"       : <nReturned>,        // if ResultsetRepresentation.ROW_COUNT is set
     *                 "result"     : <result>,           // if ResultsetRepresentation.FULL is set
     *                 "csum"       : "<16 hex digits>"   // if ResultsetRepresentation.CHECKSUM is set
     *         },
     *         ...
     *     ],
     *     ">>>totals": {
     *         "keys": <totalKeysExamined>, "docs": <totalDocsExamined>, "rows": <nReturned>
     *     }
     * }
     */

    print('{">>>commands":[');
    commands.forEach((command, index) => {
        let stringifiedCommand = JSON.stringify(command);

        for (const commandPrettyPrinter of [newlineBeforeEachStage]) {
            stringifiedCommand = commandPrettyPrinter(stringifiedCommand);
        }

        // We print the command here so that, even if the test fails,
        // we have already emitted the failing command.
        print(`{">>>command": ${stringifiedCommand},`);

        let commandToRun = {...command};
        commandToRun["cursor"] = {};
        delete commandToRun["idx"];

        // We do not use explain() as it loses the errmsg in case of an error.
        const explain = assert.commandWorked(
            db.runCommand({
                explain: commandToRun,
                verbosity: "executionStats",
            }),
        );

        const keys = sumCounters(explain, "totalKeysExamined");
        totalKeys += keys;

        const docs = sumCounters(explain, "totalDocsExamined");
        totalDocs += docs;

        const nReturned = getNReturned(explain);
        totalRows += nReturned;

        assert(
            resultsetRepresentation !== 0,
            "resultsetRepresentation must include at least one ResultsetRepresentation flag",
        );

        const showRows = (resultsetRepresentation & ResultsetRepresentation.ROW_COUNT) !== 0;
        const showFull = (resultsetRepresentation & ResultsetRepresentation.FULL) !== 0;
        const showChecksum = (resultsetRepresentation & ResultsetRepresentation.CHECKSUM) !== 0;

        let result;
        if (showFull || showChecksum) {
            result = db[command["aggregate"]].aggregate(command["pipeline"]).toArray();
        }

        // Fish for the query plan that we want to dump
        const queryPlanner =
            explain.queryPlanner !== undefined ? explain.queryPlanner : explain.stages[0]["$cursor"].queryPlanner;
        const winningPlan = queryPlanner.winningPlan;
        const queryPlan = winningPlan.queryPlan !== undefined ? [winningPlan.queryPlan] : [winningPlan];

        if (explain.stages !== undefined && explain.stages.length > 1) {
            // If there are any classic stages after the SBE stage,
            // we include them in the query plan so that they will be dumped.
            queryPlan.push(...explain.stages.slice(1));
        }

        let winningPlanString = joinPlanToString(queryPlan).trimEnd();
        for (const planPrettyPrinter of [jsonifyMultilineString]) {
            winningPlanString = planPrettyPrinter(winningPlanString);
        }

        // JSON does not allow trailing commas, so we need a different separator for the last command
        const separator = index === commands.length - 1 ? "" : ",";

        print(`    "winningPlan": [\n${winningPlanString}],`);
        print(`    "keys" : ${padNumber(keys, 9)},`);
        print(`    "docs" : ${padNumber(docs, 9)},`);

        if (showRows) {
            // JSON does not allow trailing commas, so we need to check if there are more fields after the "rows" field
            const hasMoreAfterRows = showFull || showChecksum;
            print(`    "rows" : ${padNumber(nReturned, 9)}${hasMoreAfterRows ? "," : `}${separator}`}`);
        }

        if (showFull) {
            print(`    "result" :`);
            // JSON does not allow trailing commas, so we need to check if there are more fields after the "result" field
            const hasMoreAfterResult = showChecksum;
            printjson(sortJsonStringify(result));
            print(hasMoreAfterResult ? "," : `}${separator}`);
        }

        if (showChecksum) {
            print(`    "csum" : "${resultsetChecksum(result)}"}${separator}`);
        }
        print();
    });
    print("],");

    print(
        '">>>totals": {' +
            `"commands": ${commands.length}, ` +
            `"keys": ${padNumber(totalKeys, 9)}, ` +
            `"docs": ${padNumber(totalDocs, 9)}, ` +
            `"rows": ${padNumber(totalRows, 9)}}`,
    );

    print("}");

    // Unlock the database in case it was locked just after populating the data
    assert.commandWorked(db.getMongo().getDB("admin").fsyncUnlock());
}
