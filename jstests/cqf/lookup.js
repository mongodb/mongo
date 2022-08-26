(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

load("jstests/aggregation/extras/utils.js");

const collA = db.collA;
collA.drop();

const collB = db.collB;
collB.drop();

assert.commandWorked(collA.insert({a: 1, b: 1}));
assert.commandWorked(collA.insert({a: 2, b: 2}));
assert.commandWorked(collA.insert({a: 2, b: 3}));
assert.commandWorked(collA.insert({a: 3, b: 4}));
assert.commandWorked(collA.insert({a: 5, b: 5}));
assert.commandWorked(collA.insert({a: 6, b: 6}));
assert.commandWorked(collA.insert({a: 6, b: 7}));

assert.commandWorked(collB.insert({a: 2, b: 1}));
assert.commandWorked(collB.insert({a: 3, b: 2}));
assert.commandWorked(collB.insert({a: 3, b: 3}));
assert.commandWorked(collB.insert({a: 5, b: 4}));
assert.commandWorked(collB.insert({a: 6, b: 5}));
assert.commandWorked(collB.insert({a: 6, b: 6}));
assert.commandWorked(collB.insert({a: 7, b: 7}));

{
    // Assert plan with nested fields. The top-level fields should be covered by the scans.
    const res = collA.explain("executionStats").aggregate([
        {$lookup: {from: "collB", localField: "a.a1", foreignField: "b.b1", as: "out"}}
    ]);

    const binaryJoinNode = navigateToPlanPath(res, "child.child.child");
    assertValueOnPath("BinaryJoin", binaryJoinNode, "nodeType");

    const leftScan = binaryJoinNode.leftChild.child;
    assertValueOnPath("PhysicalScan", leftScan, "nodeType");
    assert(leftScan.fieldProjectionMap.hasOwnProperty("_id"));
    assert(leftScan.fieldProjectionMap.hasOwnProperty("a"));

    const rightScan = binaryJoinNode.rightChild;
    assertValueOnPath("PhysicalScan", rightScan, "nodeType");
    assert(rightScan.fieldProjectionMap.hasOwnProperty("b"));
}

try {
    // TODO: these results need to be updated as the lookup implementation is completed. See
    // comments visitor of DocumentSourceLookUp in abt/document_source_visitor.

    // Prevent unwind and sort from being reordered with lookup.
    assert.commandWorked(
        db.adminCommand({'configureFailPoint': 'disablePipelineOptimization', 'mode': 'alwaysOn'}));

    {
        const res =
            collA
                .aggregate([
                    {$lookup: {from: "collB", localField: "a", foreignField: "a", as: "result"}},
                    {$unwind: '$result'},
                    {$project: {_id: 0, a: 1, b: 1, ra: '$result.a', rb: '$result.b'}},
                    {$project: {'result': 0}},
                    {$sort: {a: 1, b: 1, ra: 1, rb: 1}}
                ])
                .toArray();

        assert.eq(res, [
            {a: 2, b: 2, ra: 2, rb: 1},
            {a: 2, b: 3, ra: 2, rb: 1},
            {a: 3, b: 4, ra: 3, rb: 2},
            {a: 3, b: 4, ra: 3, rb: 3},
            {a: 5, b: 5, ra: 5, rb: 4},
            {a: 6, b: 6, ra: 6, rb: 5},
            {a: 6, b: 6, ra: 6, rb: 6},
            {a: 6, b: 7, ra: 6, rb: 5},
            {a: 6, b: 7, ra: 6, rb: 6}
        ]);
    }

    collA.drop();
    collB.drop();

    assert.commandWorked(collA.insert({a: [1, 2], b: 1}));

    assert.commandWorked(collB.insert({a: [2, 3], b: 1}));
    assert.commandWorked(collB.insert({a: [1, 3], b: 1}));

    {
        const res =
            collA
                .aggregate([
                    {$lookup: {from: "collB", localField: "a", foreignField: "a", as: "result"}},
                    {$unwind: '$result'},
                    {$project: {_id: 0, a: 1, b: 1, ra: '$result.a', rb: '$result.b'}},
                    {$project: {'result': 0}},
                    {$sort: {a: 1, b: 1, ra: 1, rb: 1}}
                ])
                .toArray();

        assert.eq(res,
                  [{a: [1, 2], b: 1, ra: [1, 3], rb: 1}, {a: [1, 2], b: 1, ra: [2, 3], rb: 1}]);
    }
} finally {
    assert.commandWorked(
        db.adminCommand({'configureFailPoint': 'disablePipelineOptimization', 'mode': 'off'}));
}
}());
