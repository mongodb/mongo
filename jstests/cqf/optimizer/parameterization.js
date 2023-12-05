import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    checkPlanCacheParameterization,
    removeUUIDsFromExplain,
    runWithParams
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

const coll = db.cqf_parameterization;
coll.drop();

const docs = [{a: {b: 1}}, {a: {b: 2}}, {a: {b: 3}}, {a: {b: 4}}, {a: {b: 5}}, {'': 3}];

const extraDocCount = 500;
for (let i = 0; i < extraDocCount; i++) {
    docs.push({a: {b: i + 10}});
}

assert.commandWorked(coll.insertMany(docs));

const cmds = [
    // comparison queries
    [{'a.b': 2}, 1],
    [{'a.b': {$gt: 2}}, 3 + extraDocCount],
    [{'a.b': {$gte: 2}}, 4 + extraDocCount],
    [{'a.b': {$lt: 2}}, 1],
    [{'a.b': {$lte: 2}}, 2],
    [{'': {$gt: 2}}, 1],
    // $or queries, which are translated into $in for predicates on the same path
    [{$or: [{'a.b': 1}, {'a.b': 2}]}, 2],
    [{$or: [{'a.b': 2}, {'a.b': 2}]}, 1]
];

function verifyCommandCorrectness(cmd, nReturnedExpected, find = true) {
    let res;
    if (find) {
        res = coll.explain("executionStats").find(cmd).finish();
    } else {
        res = coll.explain("executionStats").aggregate({$match: cmd});
    }
    // Correct number of documents returned.
    assert.eq(nReturnedExpected, res.executionStats.nReturned);
    // Plan uses a collection scan.
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

function verifyCommandParameterization(cmd, find = true) {
    let res;
    if (find) {
        res = runWithParams(
            [
                {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
                {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
            ],
            () => coll.explain("executionStats").find(cmd).finish());
    } else {
        res = runWithParams(
            [
                {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
                {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
            ],
            () => coll.explain("executionStats").aggregate({$match: cmd}));
    }
    // Plan is parameterized and contains FunctionCall [getParam].
    let explainStr = removeUUIDsFromExplain(db, res);
    assert(explainStr.includes("FunctionCall [getParam]"));
}

runWithParams(
    [
        // Disable fast-path since it bypasses parameterization and optimization.
        {key: "internalCascadesOptimizerDisableFastPath", value: true},
    ],
    () => {
        // Collection has no indexes except default _id index
        // Verify that queries are parameterized correctly for M2 Bonsai-eligible FIND queries
        cmds.forEach(cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1])});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0])});

        // Verify that queries are parameterized correctly for M2 Bonsai-eligible AGG queries
        cmds.forEach(cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1], false)});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], false)});
    });

// TODO SERVER-82185 Verify that M2-ineligible queries on indexed collections are not parameterized
