(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_recursive_ix_nav;
t.drop();

const valueRange = 10;
const duplicates = 3;

let expQ1 = 0;
let expQ2 = 0;
let expQ3 = 0;
let expQ4 = 0;
let expQ5 = 0;

let count = 0;
const total = valueRange * valueRange * valueRange * valueRange * valueRange * duplicates;

const bulk = t.initializeUnorderedBulkOp();
for (let va = 0; va < valueRange; va++) {
    for (let vb = 0; vb < valueRange; vb++) {
        for (let vc = 0; vc < valueRange; vc++) {
            for (let vd = 0; vd < valueRange; vd++) {
                for (let ve = 0; ve < valueRange; ve++) {
                    for (let i = 0; i < duplicates; i++) {
                        // Compute expected number of results for q1.
                        if (va === 1 && vc === 3) {
                            expQ1++;
                        }
                        // Compute expected number of results for q2.
                        if (((va >= 1 && va <= 3) || va === 6) &&
                            ((vc >= 2 && vc <= 5) || vc === 7)) {
                            expQ2++;
                        }
                        // Compute expected number of results for q3.
                        if ((va >= 1 && va <= 2) && (vb >= 3 && vb <= 4) && (vc >= 5 && vc <= 6)) {
                            expQ3++;
                        }
                        // Compute expected number of results for q4.
                        if (((va >= 1 && va <= 3) || va === 6) &&
                            ((vc >= 2 && vc <= 5) || vc === 7) &&
                            ((ve >= 3 && ve <= 4) || ve === 8)) {
                            expQ4++;
                        }
                        // Compute expected number of results for q5.
                        if (va === 1 && vc === 2) {
                            expQ5++;
                        }

                        if ((++count) % 10000 === 0) {
                            print(`${count} / ${total}`);
                        }
                        bulk.insert({a: va, b: vb, c: vc, d: vd, e: ve});
                    }
                }
            }
        }
    }
}
assert.commandWorked(bulk.execute());

assert.commandWorked(t.createIndex({a: 1, b: 1, c: 1, d: 1, e: 1}));

{
    // Test Q1.

    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerDisableScan", value: true}
        ],
        () => t.explain("executionStats").aggregate([{$match: {a: {$eq: 1}, c: {$eq: 3}}}]));
    assert.eq(expQ1, res.executionStats.nReturned);

    assertValueOnPlanPath("SpoolProducer", res, "child.leftChild.leftChild.nodeType");
    // Outer index scan for first equality prefix.
    assertValueOnPlanPath(
        "IndexScan", res, "child.leftChild.leftChild.child.children.0.child.nodeType");
    assertValueOnPlanPath(
        "SpoolConsumer", res, "child.leftChild.leftChild.child.children.1.leftChild.nodeType");
    // Inner index scan for second equality prefix.
    assertValueOnPlanPath(
        "IndexScan", res, "child.leftChild.leftChild.child.children.1.rightChild.child.nodeType");

    // Index scan for second equality prefix.
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.rightChild.nodeType");
}

{
    // Test Q2.

    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerDisableScan", value: true}
        ],
        () => t.explain("executionStats").aggregate([{
            $match: {
                $and: [
                    {$or: [{a: {$gte: 1, $lte: 3}}, {a: {$eq: 6}}]},
                    {$or: [{c: {$gte: 2, $lte: 5}}, {c: {$eq: 7}}]}
                ]
            }
        }]));
    assert.eq(expQ2, res.executionStats.nReturned);

    // We have two spool producer nodes, one for each interval for "a" ([1, 3] and [6, 6]).
    assertValueOnPlanPath(
        "SpoolProducer", res, "child.leftChild.child.children.0.leftChild.nodeType");
    assertValueOnPlanPath(1, res, "child.leftChild.child.children.0.leftChild.id");

    assertValueOnPlanPath(
        "SpoolProducer", res, "child.leftChild.child.children.1.leftChild.nodeType");
    assertValueOnPlanPath(2, res, "child.leftChild.child.children.1.leftChild.id");
}

{
    // Test Q3.

    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 3},
            {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 3},
            {key: "internalCascadesOptimizerDisableScan", value: true}
        ],
        () => t.explain("executionStats").aggregate([
            {$match: {a: {$gte: 1, $lte: 2}, b: {$gte: 3, $lte: 4}, c: {$gte: 5, $lte: 6}}},
            {$project: {_id: 0, a: 1, b: 1, c: 1}}
        ]));
    assert.eq(expQ3, res.executionStats.nReturned);

    // We have two spool producers, for the first two equality prefixes.
    assertValueOnPlanPath("SpoolProducer", res, "child.child.leftChild.nodeType");
    assertValueOnPlanPath(2, res, "child.child.leftChild.id");

    assertValueOnPlanPath("SpoolProducer", res, "child.child.rightChild.leftChild.nodeType");
    assertValueOnPlanPath(1, res, "child.child.rightChild.leftChild.id");
}

{
    // Test Q4.

    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerDisableScan", value: true}
        ],
        () => t.explain("executionStats").aggregate([
            {
                $match: {
                    $and: [
                        {$or: [{a: {$gte: 1, $lte: 3}}, {a: {$eq: 6}}]},
                        {$or: [{c: {$gte: 2, $lte: 5}}, {c: {$eq: 7}}]},
                        {$or: [{e: {$gte: 3, $lte: 4}}, {e: {$eq: 8}}]}
                    ]
                }
            },
            {$project: {_id: 0, a: 1, c: 1, e: 1}}
        ]));
    assert.eq(expQ4, res.executionStats.nReturned);

    // Assert we have two spool producers, one for each interval for "a" ([1, 3] and [6, 6]).
    assertValueOnPlanPath(
        "SpoolProducer", res, "child.child.leftChild.child.children.0.leftChild.nodeType");
    assertValueOnPlanPath(7, res, "child.child.leftChild.child.children.0.leftChild.id");

    assertValueOnPlanPath(
        "SpoolProducer", res, "child.child.leftChild.child.children.1.leftChild.nodeType");
    assertValueOnPlanPath(8, res, "child.child.leftChild.child.children.1.leftChild.id");
}

{
    // Test Q5.

    // Note reverse order sort.
    const res = runWithParams(
        [
            {key: "internalCascadesOptimizerMinIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerMaxIndexEqPrefixes", value: 2},
            {key: "internalCascadesOptimizerDisableScan", value: true},
            {key: "internalCascadesOptimizerFastIndexNullHandling", value: true}
        ],
        () => t.explain("executionStats")
                  .aggregate([{$match: {a: {$eq: 1}, c: {$eq: 2}}}, {$sort: {b: -1}}]));
    assert.eq(expQ5, res.executionStats.nReturned);

    // We have a single SpoolProducer and no collation.
    assertValueOnPlanPath("SpoolProducer", res, "child.leftChild.leftChild.nodeType");

    // Distinct scan for equality prefix. Index scans are reversed.
    assertValueOnPlanPath(
        "IndexScan", res, "child.leftChild.leftChild.child.children.0.child.nodeType");
    assertValueOnPlanPath(true, res, "child.leftChild.leftChild.child.children.0.child.reversed");
    assertValueOnPlanPath(
        "IndexScan", res, "child.leftChild.leftChild.child.children.1.rightChild.child.nodeType");
    assertValueOnPlanPath(
        true, res, "child.leftChild.leftChild.child.children.1.rightChild.child.reversed");

    // Index scan for second equality preifx. Index scan is not reversed.
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.rightChild.nodeType");
    assertValueOnPlanPath(false, res, "child.leftChild.rightChild.reversed");
}
}());
