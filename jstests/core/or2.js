(function() {
"use strict";

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

const t = db.jstests_or2;
t.drop();

function checkArrs(a, b) {
    assert.eq(a.length, b.length);
    const aStr = [];
    const bStr = [];
    a.forEach(function(x) {
        aStr.push(tojson(x));
    });
    b.forEach(function(x) {
        bStr.push(tojson(x));
    });
    for (let i = 0; i < aStr.length; ++i) {
        assert.neq(-1, bStr.indexOf(aStr[i]));
    }
}

function doTest(index) {
    if (index == null) {
        index = true;
    }

    assert.commandWorked(t.insert({_id: 0, x: 0, a: 1}));
    assert.commandWorked(t.insert({_id: 1, x: 0, a: 2}));
    assert.commandWorked(t.insert({_id: 2, x: 0, b: 1}));
    assert.commandWorked(t.insert({_id: 3, x: 0, b: 2}));
    assert.commandWorked(t.insert({_id: 4, x: 1, a: 1, b: 1}));
    assert.commandWorked(t.insert({_id: 5, x: 1, a: 1, b: 2}));
    assert.commandWorked(t.insert({_id: 6, x: 1, a: 2, b: 1}));
    assert.commandWorked(t.insert({_id: 7, x: 1, a: 2, b: 2}));

    assert.throws(function() {
        t.find({x: 0, $or: "a"}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: []}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: ["a"]}).toArray();
    });

    const a1 = t.find({x: 0, $or: [{a: 1}]}).toArray();
    checkArrs([{_id: 0, x: 0, a: 1}], a1);
    if (index) {
        const explain = t.find({x: 0, $or: [{a: 1}]}).explain();
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    }

    const a1b2 = t.find({x: 1, $or: [{a: 1}, {b: 2}]}).toArray();
    checkArrs([{_id: 4, x: 1, a: 1, b: 1}, {_id: 5, x: 1, a: 1, b: 2}, {_id: 7, x: 1, a: 2, b: 2}],
              a1b2);
    if (index) {
        const explain = t.find({x: 0, $or: [{a: 1}]}).explain();
        assert(isIxscan(db, getWinningPlan(explain.queryPlanner)));
    }
}

doTest(false);

assert(t.drop());
assert.commandWorked(t.createIndex({x: 1}));
doTest();

assert(t.drop());
assert.commandWorked(t.createIndex({x: 1, a: 1}));
doTest();

assert(t.drop());
assert.commandWorked(t.createIndex({x: 1, b: 1}));
doTest();

assert(t.drop());
assert.commandWorked(t.createIndex({x: 1, a: 1, b: 1}));
doTest();
})();
