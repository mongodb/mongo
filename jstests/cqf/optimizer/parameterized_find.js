import {
    assertValueOnNonOptimizerPlanPath,
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    checkPlanCacheParameterization
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

// TODO SERVER-82185: Remove this once M2-eligibility checker + E2E parameterization implemented
if (!checkPlanCacheParameterization(db)) {
    jsTestLog("Skipping test because E2E plan cache parameterization not yet implemented");
    quit();
}

assert.commandWorked(
    db.adminCommand({configureFailPoint: 'enableExplainInBonsai', 'mode': 'alwaysOn'}));
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

const coll = db.cqf_parameterized_find;
coll.drop();

const docs = [{a: {b: 1}}, {a: {b: 2}}, {a: {b: 3}}, {a: {b: 4}}, {a: {b: 5}}, {'': 3}];

const extraDocCount = 500;
for (let i = 0; i < extraDocCount; i++) {
    docs.push({a: {b: i + 10}});
}

assert.commandWorked(coll.insertMany(docs));

const findCmds = [
    [{'a.b': 2}, 1],
    [{'a.b': {$gt: 2}}, 3 + extraDocCount],
    [{'a.b': {$gte: 2}}, 4 + extraDocCount],
    [{'a.b': {$lt: 2}}, 1],
    [{'a.b': {$lte: 2}}, 2],
    [{'': {$gt: 2}}, 1]
];

function verifyCommandParameterization(
    findCmd, nReturnedExpected, indexed, stage = "IXSCAN", path = "inputStage.stage") {
    let res = coll.explain("executionStats").find(findCmd).finish();
    assert.eq(nReturnedExpected, res.executionStats.nReturned);
    if (indexed)
        assertValueOnNonOptimizerPlanPath(stage, res, path);
    else
        assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

// Verify that queries are parameterized correctly for M2 Bonsai-eligible find queries
// Collection has no indexes except default _id index
findCmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], cmdEl[1], false)});

// Verify that unparameterization occurs in M2 Bonsai-ineligible find queries
// Collection has indexes
assert.commandWorked(coll.createIndex({'a.b': 1}));
findCmds.slice(0, -1).forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], cmdEl[1], true)});
verifyCommandParameterization(findCmds.at(-1)[0], findCmds.at(-1)[1], true, "COLLSCAN", "stage");
