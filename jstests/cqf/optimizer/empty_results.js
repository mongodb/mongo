import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_empty_results;
t.drop();

assert.commandWorked(t.insert([{a: 1}, {a: 2}]));

const res = t.explain("executionStats").aggregate([{$match: {'a': 2}}, {$limit: 1}, {$skip: 10}]);
assert.eq(0, res.executionStats.nReturned);
assertValueOnPlanPath("CoScan", res, "child.child.child.nodeType");
