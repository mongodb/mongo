/*
 * Utility functions for Markdown golden testing.
 */

import {normalizeArray, tojsonMultiLineSortKeys} from "jstests/libs/golden_test.js";
import {code, codeOneLine, line, linebreak, subSection} from "jstests/libs/pretty_md.js";
import {formatExplainRoot, getEngine} from "jstests/libs/query/analyze_plan.js";

/**
 * Takes a collection and an aggregation pipeline. Outputs the pipeline, the aggregation results and
 * a summary of the explain to markdown. By default the results will be sorted, but the original
 * order can be kept by setting `shouldSortResults` to false.
 */
export function outputAggregationPlanAndResults(
    coll, pipeline, options = {}, shouldSortResults = true) {
    const results = coll.aggregate(pipeline, options).toArray();
    const explain = coll.explain().aggregate(pipeline, options);
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
    if (!explain.hasOwnProperty("shards")) {
        line("Execution Engine: " + getEngine(explain));
    }
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Takes a collection, the key for which to return distinct values and a filter for the distinct
 * query. Outputs the expected results, the actual returned distinct results and a summary of the
 * explain to markdown.
 */
export function outputDistinctPlanAndResults(coll, key, filter = {}, options = {}) {
    // The 'coll.distinct()' shell helper doesn't support some options like 'readConcern', even
    // though the command does support them.
    const cmdArgs = {distinct: coll.getName(), key, query: filter, ...options};
    const results = assert.commandWorked(coll.getDB().runCommand(cmdArgs)).values;
    const explain = assert.commandWorked(coll.getDB().runCommand({explain: cmdArgs}));
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
 * Helper function that manually computes the unique values for the given key in the given
 * collection (filtered on `filter`). Useful to compare with the actual output from a distinct()
 * query. Note that this function doesn't perfectly mimic actual MQL distinct semantics. For
 * example, multikey paths might not be handled properly.
 */
function getUniqueResults(coll, key, filter) {
    return Array
        .from(new Set(
            coll.find(filter, {[key]: 1, _id: 0}).toArray().flatMap(o => o ? o[key] : null)))
        .sort();
}

/**
 * Takes a collection and outputs the current state of the plan cache.
 */
export function outputPlanCacheStats(coll) {
    let stats = coll.aggregate([{$planCacheStats: {}}]).toArray();
    const fieldsToUse = ["cachedPlan", "planCacheKey", "createdFromQuery", "isActive", "shard"];
    stats.forEach(entry => {
        Object.keys(entry).forEach(field => {
            if (!fieldsToUse.includes(field)) {
                delete entry[field];
            }
        });
    });

    if (stats.every(entry => entry.hasOwnProperty('shard'))) {
        const shards = [...new Set(stats.map(entry => entry.shard))].sort();
        shards.forEach(shard => {
            subSection(shard);
            stats.filter(entry => entry.shard === shard).forEach(entry => {
                code(tojsonMultiLineSortKeys(entry));
            });
        });
    } else {
        code(tojsonMultiLineSortKeys(stats));
    }
    linebreak();
}

/*
 * Run a distinct() query and output the state of the plan cache.
 */
export function runDistinctAndOutputPlanCacheStats(coll, key, filter) {
    subSection(`Distinct on "${key}", with filter: ${tojson(filter)}`);
    assert.commandWorked(coll.runCommand('distinct', {key: key, query: filter}));
    outputPlanCacheStats(coll);
}

/*
 * Run an aggregation pipeline and output the state of the plan cache.
 */
export function runAggAndOutputPlanCacheStats(coll, pipeline) {
    subSection("Pipeline:");
    code(tojson(pipeline));
    assert.commandWorked(coll.runCommand('aggregate', {pipeline: pipeline, cursor: {}}));
    outputPlanCacheStats(coll);
}

/*
 * Run a distinct() query twice, outputting the state of the plan cache after each run. Used to
 * confirm that a single plan cache entry is held and marked as inactive/active when used for the
 * same key and filter.
 *
 * Note that this function clears the plan cache before calling the first distinct().
 */
export function validateDistinctPlanCacheUse(coll, key, filter) {
    coll.getPlanCache().clear();
    subSection("DISTINCT_SCAN stored as inactive plan");
    runDistinctAndOutputPlanCacheStats(coll, key, filter);
    subSection("DISTINCT_SCAN used as active plan");
    runDistinctAndOutputPlanCacheStats(coll, key, filter);
}

/*
 * Run an aggregation pipeline twice, outputting the state of the plan cache after each run. Used to
 * confirm that a single plan cache entry is held and marked as inactive/active when used for the
 * same pipeline.
 *
 * Note that this function clears the plan cache before calling the first aggregate().
 */
export function validateAggPlanCacheUse(coll, pipeline) {
    coll.getPlanCache().clear();
    subSection("DISTINCT_SCAN stored as inactive plan");
    runAggAndOutputPlanCacheStats(coll, pipeline);
    subSection("DISTINCT_SCAN used as active plan");
    runAggAndOutputPlanCacheStats(coll, pipeline);
}
