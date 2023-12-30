/**
 * This test verifies that the optimizer fast path is used for specific query patterns.
 * @tags: [
 *  requires_fcv_73,
 * ]
 */
import {getWinningSBEPlanFromExplain, isBonsaiFastPathPlan} from "jstests/libs/analyze_plan.js";
import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the Bonsai optimizer is not enabled.");
    quit();
}

const numRecords = 100;
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([...Array(numRecords).keys()].map(i => {
    return {_id: i, a: 1, x: {y: i, z: i}, undefinedValue: undefined};
})));

// Helper used by tests for empty queries with a simple projection.
function assertProjectedDocAndExplain(doc, explain, expectedStage, entireDocProjected) {
    assert(doc.hasOwnProperty('x'), doc);
    assert(doc['x'].hasOwnProperty('y'), doc);
    assert.eq(doc['x'].hasOwnProperty('z'), entireDocProjected, doc);
    assert(doc.hasOwnProperty('_id'), doc);

    const sbeStages = getWinningSBEPlanFromExplain(explain).stages;
    assert(sbeStages.includes(expectedStage), sbeStages);
}

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

//
// Tests for empty queries with a simple projection.
//
{
    // Empty find with simple top-level-field inclusion projection should use fast path.
    const explain = assert.commandWorked(coll.explain("executionStats").find({}, {x: 1}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    const doc = coll.find({}, {'x': 1}).toArray()[0];
    // Simple top-level-field inclusion projection should use a 'mkbson' stage rather than
    // 'project' stage.
    assertProjectedDocAndExplain(doc, explain, "mkbson", true);
}
{
    // Empty find with dotted-field inclusion projection should use fast path.
    const explain =
        assert.commandWorked(coll.explain("executionStats").find({}, {"x.y": 1}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    const doc = coll.find({}, {'x.y': 1}).toArray()[0];
    // Dotted-field inclusion projection cannot use the 'mkbson' stage.
    assertProjectedDocAndExplain(doc, explain, "project", false);
}
{
    // Empty find with dotted-field and '_id' inclusion projection should use fast path.
    const explain = assert.commandWorked(
        coll.explain("executionStats").find({}, {"x.y": 1, "_id": 1}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    const doc = coll.find({}, {'x.y': 1}).toArray()[0];
    // Dotted-field inclusion projection cannot use the 'mkbson' stage.
    assertProjectedDocAndExplain(doc, explain, "project", false);
}
{
    // Empty find with inclusion projection excluding '_id' field should use fast path.
    const explain = assert.commandWorked(
        coll.explain("executionStats").find({}, {"x.y": 1, "_id": 0}).finish());
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    const doc = coll.find({}, {'x.y': 1, "_id": 0}).toArray()[0];
    assert(doc.hasOwnProperty('x'), doc);
    assert(doc['x'].hasOwnProperty('y'), doc);
    assert(!doc['x'].hasOwnProperty('z'), doc);
    assert(!doc.hasOwnProperty('_id'), doc);

    const sbeStages = getWinningSBEPlanFromExplain(explain).stages;
    assert(sbeStages.includes("project"), sbeStages);
}
{
    // Single $project aggregate with dotted-field inclusion projection should use fast path.
    let explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {}}, {"$project": {"x.y": 1}}]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    let doc = coll.aggregate([{"$match": {}}, {"$project": {"x.y": 1}}]).toArray()[0];
    assertProjectedDocAndExplain(doc, explain, "project", false);

    doc = coll.aggregate([{"$project": {"x.y": 1}}, {"$match": {}}]).toArray()[0];
    assertProjectedDocAndExplain(doc, explain, "project", false);

    explain = coll.explain("executionStats").aggregate([{"$project": {"x.y": 1}}]);
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    doc = coll.aggregate([{"$project": {"x.y": 1}}, {"$match": {}}]).toArray()[0];
    assertProjectedDocAndExplain(doc, explain, "project", false);
}
{
    // Empty filter aggregate with dotted-field inclusion projection should use fast path.
    const explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {}}, {"$project": {"x.y": 1}}]));
    assert(isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    const doc = coll.aggregate([{"$match": {}}, {"$project": {"x.y": 1}}]).toArray()[0];
    assertProjectedDocAndExplain(doc, explain, "project", false);
}
{
    // Empty find with multiple-field inclusion projection cannot use fast path.
    let explain =
        assert.commandWorked(coll.explain("executionStats").find({}, {x: 1, y: 1}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    // Empty find with exclusion projection cannot use fast path.
    explain = assert.commandWorked(coll.explain("executionStats").find({}, {x: 0}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);
}
{
    // Empty find with non-simple inclusion projection cannot use fast path.
    let explain =
        assert.commandWorked(coll.explain("executionStats").find({}, {"x.$": 1}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {}}, {"$project": {"x": "$y"}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
    assert.eq(numRecords, explain.executionStats.nReturned);
}
{
    // Ineligible aggregates with $project cannot use fast path.
    let explain =
        assert.commandWorked(coll.explain("executionStats")
                                 .aggregate([{"$project": {"x": "$y"}}, {"$match": {"x": 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain), explain);

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$project": {"x": 1}}, {"$sort": {"x": 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain), explain);

    explain =
        assert.commandWorked(coll.explain("executionStats").aggregate([{"$project": {"x": "$y"}}]));
    assert(!isBonsaiFastPathPlan(db, explain), explain);

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {}}, {"$project": {"x": "$y"}}]));
    assert(!isBonsaiFastPathPlan(db, explain), explain);

    explain =
        assert.commandWorked(coll.explain("executionStats")
                                 .aggregate([{"$project": {"x": 1}}, {"$group": {"_id": null}}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {"x": 1}}, {"$project": {"x": 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(
        coll.explain("executionStats").aggregate([{"$match": {"x": 1}}, {"$sort": {"x": 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain =
        assert.commandWorked(coll.explain("executionStats").aggregate([{"$project": {"x": 0}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
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
    assert(isBonsaiFastPathPlan(db, explain), explain);
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
