/**
 * This test verifies that the optimizer fast path is used for specific query patterns.
 * @tags: [
 *  requires_fcv_73,
 * ]
 */
import {isBonsaiFastPathPlan} from "jstests/libs/analyze_plan.js";
import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the Bonsai optimizer is not enabled.");
    quit();
}

const numRecords = 100;
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([...Array(numRecords).keys()].map(i => {
    return {_id: i, a: 1, undefinedValue: undefined};
})));

{
    // Empty find should use the fast path.
    const explain = assert.commandWorked(coll.explain("executionStats").find().finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);
}
{
    // Empty match should use fast path.
    const explain = assert.commandWorked(coll.explain("executionStats").aggregate([{$match: {}}]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);
}
{
    // Empty aggregate should use fast path.
    const explain = assert.commandWorked(coll.explain("executionStats").aggregate([]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);
}
{
    // Find with predicates should not use a fast path.
    const explain =
        assert.commandWorked(coll.explain("executionStats").find({a: 1, b: 2}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Agg with matches should not use a fast path.
    const explain =
        assert.commandWorked(coll.explain("executionStats").aggregate([{$match: {a: 1, b: 2}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Agg with matches should not use a fast path.
    const explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{$match: {a: 1}}, {$match: {b: 2}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Agg with an empty and a non-emtpy $match in the pipeline to ensure that the pattern matching
    // uses the full query and not only the first part of the pipeline.
    const explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{$match: {}}, {$match: {b: 2}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Find with equality check on a top-level field should use fast path.
    let explain = assert.commandWorked(coll.explain("executionStats").find({a: 1}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    explain = assert.commandWorked(coll.explain("executionStats").find({nonexistent: 1}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Agg with equality check on a top-level field should use fast path.
    let explain =
        assert.commandWorked(coll.explain("executionStats").aggregate([{$match: {a: 1}}]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{$match: {nonexistent: 1}}]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}
{
    // Equality with null should use fast path but should not match values which are explicitly
    // undefined.
    const explain =
        assert.commandWorked(coll.explain("executionStats").find({undefinedValue: null}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(0, explain.executionStats.nReturned);
}

// Test single predicate fast path with different constants and comparison ops.
const constantValues = [
    NaN,
    null,
    "str",
    {b: 1},
    [{}, {}],
    123,
    1.23,
    [1, 2, 3],
    [1, 2, [3, 4]],
    new Date(),
];

const comparisonOps = ["$eq", "$lt", "$lte", "$gt", "$gte"];

for (const value of constantValues) {
    for (const op of comparisonOps) {
        const predicate = {[op]: value};
        jsTestLog(`Testing single predicate fast path with predicate ${tojson(predicate)}`);

        {
            // Find with single predicate on a top-level field should use fast path.
            const explain = assert.commandWorked(
                coll.explain("executionStats").find({a: {...predicate}}).finish());
            assert(isBonsaiFastPathPlan(db, explain));
        }
        {
            // Agg with single predicate on a top-level field should use fast path.
            const explain = assert.commandWorked(
                coll.explain("executionStats").aggregate([{$match: {a: {...predicate}}}]));
            assert(isBonsaiFastPathPlan(db, explain));
        }
    }
}
