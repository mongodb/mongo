(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_explain_test;
t.drop();

assert.commandWorked(t.insert({_id: 3, a: 1}));
assert.commandWorked(t.createIndex({a: 1}));

// Demonstrate we can obtain a V2 explain.
const res = runWithParams(
    [
        {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
        {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
    ],
    () => t.explain("executionStats").aggregate([{$match: {a: 2, b: 3}}]));

const expectedStr =
    `Root [{scan_0}]
Filter []
|   EvalFilter []
|   |   Variable [evalTemp_3]
|   PathTraverse [1]
|   PathCompare [Eq]
|   Const [3]
Filter []
|   EvalFilter []
|   |   Variable [evalTemp_2]
|   PathCompare [Eq]
|   Const [2]
PhysicalScan [{'<root>': scan_0, 'a': evalTemp_2, 'b': evalTemp_3}, cqf_explain_test_]
`;

const actualStr = removeUUIDsFromExplain(db, res);
assert.eq(expectedStr, actualStr);
}());
