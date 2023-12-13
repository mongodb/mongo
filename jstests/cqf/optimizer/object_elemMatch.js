import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_object_elemMatch;

t.drop();
assert.commandWorked(t.insert({a: [{a: 1, b: 1}, {a: 1, b: 2}]}));
assert.commandWorked(t.insert({a: [{a: 2, b: 1}, {a: 2, b: 2}]}));
assert.commandWorked(t.insert({a: {a: 2, b: 1}}));
assert.commandWorked(t.insert({a: [{b: [1, 2], c: [3, 4]}]}));
assert.commandWorked(t.insert({a: [{"": [1, 2], c: [3, 4]}]}));

{
    // Object elemMatch. Currently we do not support index here.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {a: 2, b: 1}}}}]));
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.child.nodeType");
}

{
    // When Sargable is disabled, we expect to have a single Filter node instead of two.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: true}],
        () => t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {a: 2, b: 1}}}}]));
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

{
    // Should not be getting any results.
    const res = runWithParams(
        [{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}],
        () => t.explain("executionStats").aggregate([
            {$match: {a: {$elemMatch: {b: {$elemMatch: {}}, c: {$elemMatch: {}}}}}}
        ]));
    assert.eq(0, res.executionStats.nReturned);
}

{
    // Should not be getting any results.
    const res =
        runWithParams([{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: true}],
                      () => t.explain("executionStats").aggregate([
                          {$match: {a: {$elemMatch: {b: {$elemMatch: {}}, c: {$elemMatch: {}}}}}}
                      ]));
    assert.eq(0, res.executionStats.nReturned);
}

{
    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false},
        ],
        () => t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {"": 1}}}}]));
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.child.nodeType");
}

{
    // When Sargable is disabled, we expect to have a single Filter node instead of two.
    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: true},
        ],
        () => t.explain("executionStats").aggregate([{$match: {a: {$elemMatch: {"": 1}}}}]));
    assert.eq(1, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
