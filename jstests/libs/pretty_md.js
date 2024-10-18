/**
 * Provides helper functions to output content to markdown. This is used for golden testing, using
 * `printGolden` to write to the expected output files.
 */

/* eslint-disable no-undef */
import {normalizeArray, tojsonMultiLineSortKeys} from "jstests/libs/golden_test.js";
import {
    formatExplainRoot,
} from "jstests/libs/query/analyze_plan.js";

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

export function code(msg, fmt = "json") {
    printGolden("```" + fmt);
    printGolden(msg);
    printGolden("```");
}

export function linebreak() {
    printGolden();
}

/**
 * Takes a collection and an aggregation pipeline. Outputs the pipeline, the aggregation results and
 * a summary of the explain to markdown. By default the results will be sorted, but the original
 * order can be kept by setting `shouldSortResults` to false.
 */
export function outputAggregationPlanAndResults(
    coll, pipeline, options = {}, shouldSortResults = true) {
    const results = coll.aggregate(pipeline, options).toArray();
    const explain = coll.explain("allPlansExecution").aggregate(pipeline, options);
    const flatPlan = formatExplainRoot(explain);

    subSection("Pipeline");
    code(tojson(pipeline));

    if (Object.keys(options).length > 0) {
        subSection("Options");
        code(tojson(options));
    }

    subSection("Results");
    code(normalizeArray(results, shouldSortResults));

    subSection("Summarized explain");
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Takes a collection, the key for which to return distinct values and a filter for the distinct
 * query. Outputs the expected results, the actual returned distinct results and a summary of the
 * explain to markdown.
 */
export function outputDistinctPlanAndResults(coll, key, filter = {}, options = {}) {
    const results = coll.distinct(key, filter, options);
    const explain = coll.explain("allPlansExecution").distinct(key, filter, options);
    const flatPlan = formatExplainRoot(explain);

    subSection(`Distinct on "${key}", with filter: ${tojson(filter)}${
        Object.keys(options).length ? `, and options: ${tojson(options)}` : ''}`);

    subSection("Expected results");
    codeOneLine(getUniqueResults(coll, key, filter));

    subSection("Distinct results");
    codeOneLine(results);

    subSection("Summarized explain");
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Takes a collection and outputs the current state of the plan cache.
 */
export function outputPlanCacheStats(coll) {
    let stats = coll.aggregate([{$planCacheStats: {}}]).toArray();
    const fieldsToUse = ["cachedPlan", "createdFromQuery", "isActive"];
    stats.forEach(entry => {
        Object.keys(entry).forEach(field => {
            if (!fieldsToUse.includes(field)) {
                delete entry[field];
            }
        });
    });
    code(tojsonMultiLineSortKeys(stats));
    linebreak();
}

/**
 * Helper function that manually computes the unique values for the given key in the given
 * collection (filtered on `filter`). Useful to compare with the actual output from a distinct()
 * query. Note that this function doesn't perfectly mimic actual MQL distinct semantics. For
 * example, multikey paths might not be handled properly.
 */
function getUniqueResults(coll, key, filter) {
    return Array
        .from(new Set(coll.find(filter, {[key]: 1, _id: 0}).toArray().map(o => o ? o[key] : null)))
        .sort();
}
