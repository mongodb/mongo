(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For assertArrayEq.

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_agg_expr;

{
    t.drop();
    assert.commandWorked(t.insert({a: "a1", b: "b1", c: "c1"}));

    const res =
        t.aggregate([{$project: {concat: {$concat: ["$a", " - ", "$b", " - ", "$c"]}}}]).toArray();

    assert.eq(1, res.length);
    assert.eq("a1 - b1 - c1", res[0].concat);
}

{
    t.drop();
    assert.commandWorked(t.insert({a: 5, b: 10, c: 20, d: 25, e: -5, f: 2.4}));

    const res = t.aggregate([{
                     $project: {
                         res1: {$divide: ["$a", "$b"]},
                         res2: {$divide: ["$c", "$a"]},
                         res3: {$mod: ["$d", "$b"]},
                         res4: {$abs: "$e"},
                         res5: {$floor: "$f"},
                         res6: {$ceil: {$ln: "$d"}}
                     }
                 }]).toArray();

    assert.eq(1, res.length);
    assert.eq(0.5, res[0].res1);
    assert.eq(4, res[0].res2);
    assert.eq(5, res[0].res3);
    assert.eq(5, res[0].res4);
    assert.eq(2, res[0].res5);
    assert.eq(4, res[0].res6);
}

{
    t.drop();
    assert.commandWorked(t.insert({a: 1, b: [{c: 2}, {c: 3}]}));
    assert.commandWorked(t.insert({a: 1, b: [[{c: 2}, {c: 3}]]}));

    const res = t.aggregate([{$project: {a: "$b.c"}}]).toArray();

    assert.eq(2, res.length);
    assert.eq([2, 3], res[0].a);

    // TODO: SERVER-67153: Clarify behavior of array traversal in agg expression.
    assert.eq([[2, 3]], res[1].a);
}

{
    t.drop();
    assert.commandWorked(t.insert({_id: 0, a: {b: 1}}));
    assert.commandWorked(t.insert({_id: 1, a: [{b: 1}]}));
    assert.commandWorked(t.insert({_id: 2, a: [[{b: 1}]]}));

    assert.commandWorked(t.insert({_id: 3, a: {b: [1]}}));
    assert.commandWorked(t.insert({_id: 4, a: [{b: [1]}]}));
    assert.commandWorked(t.insert({_id: 5, a: [[{b: [1]}]]}));

    {
        const res = t.aggregate([{$match: {$expr: {$eq: ['$a.b', 1]}}}]).toArray();

        assert.eq(1, res.length);
        assert.eq({b: 1}, res[0].a);
    }
    {
        const res = t.aggregate([{$match: {$expr: {$eq: ['$a.b', [1]]}}}]).toArray();
        assertArrayEq({actual: res, expected: [{_id: 1, a: [{b: 1}]}, {_id: 3, a: {b: [1]}}]});
    }
}
{
    t.drop();
    assert.commandWorked(t.insert({_id: 0, a: 1}));
    assert.commandWorked(t.insert({_id: 1, a: 2}));
    assert.commandWorked(t.insert({_id: 2, a: 3}));

    {
        const res = t.aggregate([{$match: {$expr: {$lt: [2, "$a"]}}}]).toArray();

        assert.eq(1, res.length);
        assert.eq(3, res[0].a);
    }
    {
        const res = t.aggregate([{$match: {$expr: {$gt: ["$a", 2]}}}]).toArray();

        assert.eq(1, res.length);
        assert.eq(3, res[0].a);
    }
    {
        const res = t.aggregate([{$match: {$expr: {$lte: [2, "$a"]}}}]).toArray();

        assert.eq(2, res.length);
        assertArrayEq({actual: res, expected: [{_id: 1, a: 2}, {_id: 2, a: 3}]});
    }
    {
        const res = t.aggregate([{$match: {$expr: {$gte: ["$a", 2]}}}]).toArray();

        assert.eq(2, res.length);
        assertArrayEq({actual: res, expected: [{_id: 1, a: 2}, {_id: 2, a: 3}]});
    }
    {
        const res = t.aggregate([{$match: {$expr: {$gt: [3, "$a"]}}}]).toArray();

        assert.eq(2, res.length);
        assertArrayEq({actual: res, expected: [{_id: 0, a: 1}, {_id: 1, a: 2}]});
    }
    {
        const res = t.aggregate([{$match: {$expr: {$lt: ["$a", 3]}}}]).toArray();

        assert.eq(2, res.length);
        assertArrayEq({actual: res, expected: [{_id: 0, a: 1}, {_id: 1, a: 2}]});
    }
    {
        const res = t.aggregate([{$match: {$expr: {$gte: [3, "$a"]}}}]).toArray();

        assert.eq(3, res.length);
        assertArrayEq({actual: res, expected: [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}]});
    }
    {
        const res = t.aggregate([{$match: {$expr: {$lte: ["$a", 3]}}}]).toArray();

        assert.eq(3, res.length);
        assertArrayEq({actual: res, expected: [{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}]});
    }
}
}());
