import {
    checkCascadesOptimizerEnabled,
    navigateToPlanPath,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const t = db.cqf_project_expr_dependency;
t.drop();

for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a1: i, b1: i, c1: i}));
}

{
    const res = runWithParams(
        [
            // TODO SERVER-83937: Because we are not generating a SargableNode here, we are not
            // pushing down the EvaluationNode into the PhysicalScan.
            {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}
        ],
        () => t.explain("executionStats").aggregate([
            {$addFields: {x: '$a1', y: '$b1', z: '$c1'}},
            {$addFields: {a: {$add: ["$x", "$y"]}, b: {$add: ["$y", "$z"]}}},
            {$project: {_id: 0, out: "$b"}}
        ]));

    // Demonstrate we only need to read "b1" and "c1" from the collection.
    const scanNodeProjFieldMap = navigateToPlanPath(res, "child.child.fieldProjectionMap");
    assert.eq(["b1", "c1"], Object.keys(scanNodeProjFieldMap));
}
