/*
 * Utility functions for Markdown golden testing.
 */

import {normalizeArray, tojsonMultiLineSortKeys} from "jstests/libs/golden_test.js";
import {code, codeOneLine, line, linebreak, subSection} from "jstests/libs/pretty_md.js";
import {formatExplainRoot, getEngine, getStableExecutionStats} from "jstests/libs/query/analyze_plan.js";

/**
 * Helper that ensures limit and/or skip appear in the explain output if specified. Also prints out
 * common explain output for queries that specify limit/skip.
 */
function outputCommonPlanAndResults({querySection, resultsSection, explain, expected}) {
    const executionStages = explain.executionStats.executionStages;

    // Verify expected results
    assert.eq(executionStages.stage, expected.stage);
    if (expected.limit !== undefined) {
        assert.eq(executionStages.limitAmount, expected.limit);
    }
    if (expected.skip !== undefined) {
        assert.eq(executionStages.skipAmount, expected.skip);
    }

    // Get stable fields from executionStats section of explain output
    const flatPlan = getStableExecutionStats(explain);

    subSection("Query");
    code(querySection);

    subSection("Results");
    code(resultsSection);

    subSection("Summarized explain executionStats");
    if (!explain.hasOwnProperty("shards")) {
        line("Execution Engine: " + getEngine(explain));
    }
    code(tojsonMultiLineSortKeys(flatPlan));

    linebreak();
}

/**
 * Takes a collection and a cursor for a find query. Outputs the query, results and a summary of the
 * explain to markdown. By default the results will be sorted, but the original order can be kept by
 * setting `shouldSortResults` to false.
 */
export function outputFindPlanAndResults(cursor, expected, shouldSortResults = true) {
    const results = cursor.toArray();
    const explain = cursor.explain("executionStats");
    const executionStages = explain.executionStats.executionStages;

    const actualReturned = results.length;
    assert.eq(actualReturned, explain.executionStats.nReturned);
    assert.eq(actualReturned, executionStages.nReturned);

    outputCommonPlanAndResults({
        querySection: tojson(cursor._convertToCommand()),
        resultsSection: normalizeArray(results, shouldSortResults),
        explain,
        expected,
    });
}

/**
 * Takes a count command, explain output, expected explain fields, and count result. Outputs the
 * query, results and a summary of the explain to markdown.
 */
export function outputCountPlanAndResults(cmdObj, explain, expected, actualCount) {
    const executionStages = explain.executionStats.executionStages;

    assert.eq(actualCount, executionStages.nCounted);

    outputCommonPlanAndResults({querySection: tojson(cmdObj), resultsSection: actualCount, explain, expected});
}

/**
 * Takes a collection and an aggregation pipeline. Outputs the pipeline, the aggregation results and
 * a summary of the explain to markdown. By default the results will be sorted, but the original
 * order can be kept by setting `shouldSortResults` to false.
 */
export function outputAggregationPlanAndResults(coll, pipeline, options = {}, shouldSortResults = true) {
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

    subSection(
        `Distinct on "${key}", with filter: ${tojson(filter)}${
            Object.keys(options).length ? `, and options: ${tojson(options)}` : ""
        }`,
    );

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
    const fieldsToUse = ["cachedPlan", "planCacheKey", "createdFromQuery", "isActive", "shard"];
    stats.forEach((entry) => {
        Object.keys(entry).forEach((field) => {
            if (!fieldsToUse.includes(field)) {
                delete entry[field];
            }
        });
    });

    if (stats.every((entry) => entry.hasOwnProperty("shard"))) {
        const shards = [...new Set(stats.map((entry) => entry.shard))].sort();
        shards.forEach((shard) => {
            subSection(shard);
            stats
                .filter((entry) => entry.shard === shard)
                .forEach((entry) => {
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
    assert.commandWorked(coll.runCommand("distinct", {key: key, query: filter}));
    outputPlanCacheStats(coll);
}

/*
 * Run an aggregation pipeline and output the state of the plan cache.
 */
export function runAggAndOutputPlanCacheStats(coll, pipeline) {
    subSection("Pipeline:");
    code(tojson(pipeline));
    assert.commandWorked(coll.runCommand("aggregate", {pipeline: pipeline, cursor: {}}));
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
