/**
 * Test how $setWindowFields desugars.
 *
 * We handle partitionBy and sortBy by generating a separate $sort stage.
 *
 * @tags: [
 *   # We assume the pipeline is not split into a shardsPart and mergerPart.
 *   assumes_unsharded_collection,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 *   # This feature flag adjusts the desugaring a bit - requesting 'outputSortKeyMetadata' from the
 *   # $sort stage.
 *   featureFlagRankFusionBasic,
 *   requires_fcv_81,
 * ]
 */
import {getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
assert.commandWorked(coll.insert({}));

// Use .explain() to see what the stage desugars to.
// The result is formatted as explain-output, which differs from MQL syntax in some cases:
// for example {$sort: {a: 1}} explains as {$sort: {sortKey: {a: 1}}}.
function desugar(stage) {
    const result = coll.explain().aggregate([
        // prevent stages from being absorbed into the .find() layer
        {$_internalInhibitOptimization: {}},
        stage,
    ]);
    assert.commandWorked(result);
    const explain = getSingleNodeExplain(result);

    assert(Array.isArray(explain.stages), explain);
    // The first two stages should be the .find() cursor and the inhibit-optimization stage;
    // the rest of the stages are what the user's 'stage' expanded to.
    assert(explain.stages[0].$cursor, explain);
    assert(explain.stages[1].$_internalInhibitOptimization, explain);
    return explain.stages.slice(2);
}

// Often, the desugared stages include a generated temporary name.
// When this happens, it's always in the first stage, an $addFields.
function extractTmp(stages) {
    assert(stages[0].$addFields, stages);
    const tmp = Object.keys(stages[0].$addFields)[0];
    assert(tmp, stages);
    return tmp;
}

// No partitionBy and no sortBy means we don't need to sort the input.
assert.eq(desugar({$setWindowFields: {output: {}}}), [{$_internalSetWindowFields: {output: {}}}]);

// 'sortBy' becomes an explicit $sort stage.
assert.eq(desugar({$setWindowFields: {sortBy: {ts: 1}, output: {}}}), [
    {$sort: {sortKey: {ts: 1}, outputSortKeyMetadata: true}},
    {$_internalSetWindowFields: {sortBy: {ts: 1}, output: {}}},
]);

// 'partitionBy' a field becomes an explicit $sort stage.
assert.eq(desugar({$setWindowFields: {partitionBy: "$zip", output: {}}}), [
    {$sort: {sortKey: {zip: 1}, outputSortKeyMetadata: true}},
    {$_internalSetWindowFields: {partitionBy: "$zip", output: {}}},
]);

// 'partitionBy' an expression becomes $set + $sort + $unset.
// Also, the _internal stage reads from the already-computed field.
let stages = desugar({$setWindowFields: {partitionBy: {$toLower: "$country"}, output: {}}});
let tmp = extractTmp(stages);
assert.eq(stages, [
    {$addFields: {[tmp]: {$toLower: ["$country"]}}},
    {$sort: {sortKey: {[tmp]: 1}, outputSortKeyMetadata: true}},
    {$_internalSetWindowFields: {partitionBy: "$" + tmp, output: {}}},
    {$project: {[tmp]: false, _id: true}},
]);

// $sort first by partitionBy, then sortBy, because we sort within each partition.
assert.eq(desugar({$setWindowFields: {partitionBy: "$zip", sortBy: {ts: -1, _id: 1}, output: {}}}), [
    {$sort: {sortKey: {zip: 1, ts: -1, _id: 1}, outputSortKeyMetadata: true}},
    {$_internalSetWindowFields: {partitionBy: "$zip", sortBy: {ts: -1, _id: 1}, output: {}}},
]);

stages = desugar({
    $setWindowFields: {partitionBy: {$toLower: "$country"}, sortBy: {ts: -1, _id: 1}, output: {}},
});
tmp = extractTmp(stages);
assert.eq(stages, [
    {$addFields: {[tmp]: {$toLower: ["$country"]}}},
    {$sort: {sortKey: {[tmp]: 1, ts: -1, _id: 1}, outputSortKeyMetadata: true}},
    {$_internalSetWindowFields: {partitionBy: "$" + tmp, sortBy: {ts: -1, _id: 1}, output: {}}},
    {$project: {[tmp]: false, _id: true}},
]);
