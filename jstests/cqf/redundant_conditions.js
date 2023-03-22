/**
 * Tests scenario related to SERVER-22857.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_redundant_condition;
t.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i}));
}

{
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => t.explain("executionStats").find({$and: [{a: 1}, {a: 1}]}).finish());
    assert.eq(1, res.executionStats.nReturned);

    // The condition on "a" is not repeated.
    const expectedStr =
        `Root [{scan_0}]
Filter []
|   EvalFilter []
|   |   Variable [evalTemp_2]
|   PathTraverse [1]
|   PathCompare [Eq]
|   Const [1]
PhysicalScan [{'<root>': scan_0, 'a': evalTemp_2}, cqf_redundant_condition_]
`;
    const actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);
}
}());
