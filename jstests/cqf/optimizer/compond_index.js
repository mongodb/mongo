(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_compound_index;
t.drop();

const bulk = t.initializeUnorderedBulkOp();
for (var va = 0; va < 1000; va++) {
    for (var vb = 0; vb < 10; vb++) {
        bulk.insert({a: va, b: vb});
    }
}

assert.commandWorked(bulk.execute());
assert.commandWorked(t.createIndex({a: 1, b: 1}));

{
    // Collection scan: a = 1.
    const res =
        runWithParams([{key: "internalCascadesOptimizerDisableIndexes", value: true}],
                      () => t.explain("executionStats").aggregate([{$match: {a: {$eq: 1}}}]));
    assert.eq(10, res.executionStats.nReturned);
}
{
    // Collection scan: a > 1 and a < 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableIndexes", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gt: 1, $lt: 3}}}]));
    assert.eq(10, res.executionStats.nReturned);
}
{
    // Collection scan: a >= 1 and a < 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableIndexes", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gte: 1, $lt: 3}}}]));
    assert.eq(20, res.executionStats.nReturned);
}
{
    // Collection scan: a > 1 and a <= 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableIndexes", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gt: 1, $lte: 3}}}]));
    assert.eq(20, res.executionStats.nReturned);
}
{
    // Collection scan: a >= 1 and a <= 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableIndexes", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gte: 1, $lte: 3}}}]));
    assert.eq(30, res.executionStats.nReturned);
}

{
    // Index scan: a = 1.
    const res =
        runWithParams([{key: "internalCascadesOptimizerDisableScan", value: true}],
                      () => t.explain("executionStats").aggregate([{$match: {a: {$eq: 1}}}]));
    assert.eq(10, res.executionStats.nReturned);
}
{
    // Index scan: a > 1 and a < 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableScan", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gt: 1, $lt: 3}}}]));
    assert.eq(10, res.executionStats.nReturned);
}
{
    // Index scan: a >= 1 and a < 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableScan", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gte: 1, $lt: 3}}}]));
    assert.eq(20, res.executionStats.nReturned);
}
{
    // Index scan: a > 1 and a <= 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableScan", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gt: 1, $lte: 3}}}]));
    assert.eq(20, res.executionStats.nReturned);
}
{
    // Index scan: a >= 1 and a <= 3.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableScan", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$gte: 1, $lte: 3}}}]));
    assert.eq(30, res.executionStats.nReturned);
}
}());
