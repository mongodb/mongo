t = db.jstests_or3;
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

checkArrs = function(a, b) {
    assert.eq(a.length, b.length);
    aStr = [];
    bStr = [];
    a.forEach(function(x) {
        aStr.push(tojson(x));
    });
    b.forEach(function(x) {
        bStr.push(tojson(x));
    });
    for (i = 0; i < aStr.length; ++i) {
        assert.neq(-1, bStr.indexOf(aStr[i]));
    }
};

doTest = function(index) {
    if (index == null) {
        index = true;
    }

    t.save({_id: 0, x: 0, a: 1});
    t.save({_id: 1, x: 0, a: 2});
    t.save({_id: 2, x: 0, b: 1});
    t.save({_id: 3, x: 0, b: 2});
    t.save({_id: 4, x: 1, a: 1, b: 1});
    t.save({_id: 5, x: 1, a: 1, b: 2});
    t.save({_id: 6, x: 1, a: 2, b: 1});
    t.save({_id: 7, x: 1, a: 2, b: 2});

    assert.throws(function() {
        t.find({x: 0, $nor: "a"}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: []}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: ["a"]}).toArray();
    });

    an1 = t.find({$nor: [{a: 1}]}).toArray();
    checkArrs(t.find({a: {$ne: 1}}).toArray(), an1);

    an1bn2 = t.find({x: 1, $nor: [{a: 1}, {b: 2}]}).toArray();
    checkArrs([{_id: 6, x: 1, a: 2, b: 1}], an1bn2);
    checkArrs(t.find({x: 1, a: {$ne: 1}, b: {$ne: 2}}).toArray(), an1bn2);
    if (index) {
        var explain = t.find({x: 1, $nor: [{a: 1}, {b: 2}]}).explain();
        assert(isIxscan(explain.queryPlanner.winningPlan));
    }

    an1b2 = t.find({$nor: [{a: 1}], $or: [{b: 2}]}).toArray();
    checkArrs(t.find({a: {$ne: 1}, b: 2}).toArray(), an1b2);
};

doTest(false);

t.ensureIndex({x: 1});
doTest();

t.drop();
t.ensureIndex({x: 1, a: 1});
doTest();

t.drop();
t.ensureIndex({x: 1, b: 1});
doTest();

t.drop();
t.ensureIndex({x: 1, a: 1, b: 1});
doTest();
