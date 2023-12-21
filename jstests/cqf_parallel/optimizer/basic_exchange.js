import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_exchange;
t.drop();

assert.commandWorked(t.insert({a: {b: 1}}));
assert.commandWorked(t.insert({a: {b: 2}}));
assert.commandWorked(t.insert({a: {b: 3}}));
assert.commandWorked(t.insert({a: {b: 4}}));
assert.commandWorked(t.insert({a: {b: 5}}));

const runTest =
    () => {
        const res = t.explain("executionStats").aggregate([{$match: {'a.b': 2}}]);
        assert.eq(1, res.executionStats.nReturned);
        assertValueOnPlanPath("Exchange", res, "child.nodeType");
    }

// Test exchange with both Sargable nodes & Filter nodes
runWithParams([{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}],
              runTest);
runWithParams([{key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: true}],
              runTest);
