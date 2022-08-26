(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.

const t = db.cqf_type_bracket;
t.drop();

// Generate enough documents for index to be preferable if it exists.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i}));
    assert.commandWorked(t.insert({a: i.toString()}));
}

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: "2"}}}]);
    assert.eq(12, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: "95"}}}]);
    assert.eq(4, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: 2}}}]);
    assert.eq(2, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: 95}}}]);
    assert.eq(4, res.executionStats.nReturned);
    assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
}

assert.commandWorked(t.createIndex({a: 1}));

{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: "2"}}}]);
    assert.eq(12, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: "95"}}}]);
    assert.eq(4, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: 2}}}]);
    assert.eq(2, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
{
    const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: 95}}}]);
    assert.eq(4, res.executionStats.nReturned);
    assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
}
}());