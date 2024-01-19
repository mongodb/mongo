import {
    assertValueOnPlanPath,
    checkCascadesOptimizerEnabled,
    removeUUIDsFromExplain,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
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

/**
 * Verify that the command returns the correct number of documents and produces a correct plan.
 * 'nReturnedExpected' is the expected number of documents returned from the command. 'find` is true
 * if it is a find command, and false if it is an aggregate command. 'assertPhysicalScan` is true if
 * the command is M2-eligible and produces a collection scan plan, false if the command is
 * M2-ineligible.
 */
function verifyCommandCorrectness(cmd, nReturnedExpected, find, assertPhysicalScan) {
    let res;
    if (find) {
        res = coll.explain("executionStats").find(cmd).finish();
    } else {
        res = coll.explain("executionStats").aggregate({$match: cmd});
    }
    // Correct number of documents returned.
    assert.eq(nReturnedExpected, res.executionStats.nReturned);

    if (assertPhysicalScan) {
        // Plan uses a collection scan.
        assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
    }
}

/**
 * Verify that the command is parameterized if it is M2-eligible. 'find` is true if it is a find
 * command, false if it is an aggregate command. 'assertParameterized` is true if the command is
 * M2-eligible and contains FunctionCall [getParam] nodes, false if the command is M2-ineligible.
 */
function verifyCommandParameterization(cmd, find, assertParameterized) {
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

    let explainStr = removeUUIDsFromExplain(db, res);
    if (assertParameterized) {
        // Plan is parameterized and contains FunctionCall [getParam].
        assert(explainStr.includes("FunctionCall [getParam]"));
    } else {
        assert(!explainStr.includes("FunctionCall [getParam]"));
    }
}

runWithParams(
    [
        // Disable fast-path since it bypasses parameterization and optimization.
        {key: "internalCascadesOptimizerDisableFastPath", value: true},
    ],
    () => {
        const find = true;
        const agg = false;
        const assertPhysicalScan = true;
        const assertParamerized = true;

        // Collection has no indexes except default _id index
        // Verify that queries are parameterized correctly for M2 Bonsai-eligible FIND queries
        cmds.forEach(
            cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1], find, assertPhysicalScan)});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], find, assertParamerized)});

        // Verify that queries are parameterized correctly for M2 Bonsai-eligible AGG queries
        cmds.forEach(
            cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1], agg, assertPhysicalScan)});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], agg, assertParamerized)});

        assert.commandWorked(coll.createIndex({'a.b': 1}));
        // Collection has indexes
        // Verify that queries are not parameterized for M2 Bonsai-ineligible FIND queries
        cmds.forEach(
            cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1], find, !assertPhysicalScan)});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], find, !assertParamerized)});

        // Verify that queries are not parameterized for M2 Bonsai-ineligible AGG queries
        cmds.forEach(
            cmdEl => {verifyCommandCorrectness(cmdEl[0], cmdEl[1], agg, !assertPhysicalScan)});
        cmds.forEach(cmdEl => {verifyCommandParameterization(cmdEl[0], agg, !assertParamerized)});
    });
