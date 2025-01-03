/**
 * Provides helper functions to output content to markdown. This is used for golden testing, using
 * `printGolden` to write to the expected output files.
 */

/* eslint-disable no-undef */
import {tojsonMultiLineSortKeys} from "jstests/libs/golden_test.js";

let sectionCount = 1;
export function section(msg) {
    printGolden(`## ${sectionCount}.`, msg);
    sectionCount++;
}

export function subSection(msg) {
    printGolden("###", msg);
}

export function line(msg) {
    printGolden(msg);
}

export function codeOneLine(msg) {
    printGolden("`" + tojsononeline(msg) + "`");
}

export function note(msg) {
    printGolden("> [!NOTE]");
    printGolden("> " + msg);
}

export function code(msg, fmt = "json") {
    printGolden("```" + fmt);
    printGolden(msg);
    printGolden("```");
}

export function linebreak() {
    printGolden();
}

function stripFields(obj, fields) {
    if (typeof obj === 'object') {
        for (let name of fields) {
            delete obj[name];
        }
        for (let value of Object.values(obj)) {
            stripFields(value, fields);
        }
    } else if (Array.isArray(obj)) {
        for (let elem in obj) {
            stripFields(elem, fields);
        }
    }
}

/**
 * Format the explain result for a given find query.
 */
export function outputShardedFindSummaryAndResults(queryObj) {
    const explain = queryObj.explain();
    const winningPlan = explain.queryPlanner.winningPlan;

    subSection(`Find : "${tojson(queryObj._filter)}", additional params: ${
        tojson(queryObj._additionalCmdParams)}`);

    subSection("Stage");
    codeOneLine(winningPlan.stage);

    subSection("Shard winning plans");
    let previous;
    for (let shard of winningPlan.shards) {
        // Most queries will result in identical plans across shards.
        // To minimise visual clutter when reviewing golden output, de-dupe
        // identical shard results.
        let shardPlan = shard.winningPlan.queryPlan || shard.winningPlan;
        // Strip out fields which vary with and without feature flags for a consistent
        // golden output.
        stripFields(shardPlan, ["isCached", "planNodeId"]);
        let current = tojsonMultiLineSortKeys(shardPlan);
        if (previous != current) {
            code(current);
            previous = current;
        }
    }

    subSection("Results");
    let res = queryObj.toArray();
    code(tojson(res));
    linebreak();
    return res;
}
