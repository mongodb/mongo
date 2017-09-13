
t = db.index_check2;
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

for (var i = 0; i < 1000; i++) {
    var a = [];
    for (var j = 1; j < 5; j++) {
        a.push("tag" + (i * j % 50));
    }
    t.save({num: i, tags: a});
}

q1 = {
    tags: "tag6"
};
q2 = {
    tags: "tag12"
};
q3 = {
    tags: {$all: ["tag6", "tag12"]}
};

assert.eq(120, t.find(q1).itcount(), "q1 a");
assert.eq(120, t.find(q2).itcount(), "q2 a");
assert.eq(60, t.find(q3).itcount(), "q3 a");

t.ensureIndex({tags: 1});

assert.eq(120, t.find(q1).itcount(), "q1 a");
assert.eq(120, t.find(q2).itcount(), "q2 a");
assert.eq(60, t.find(q3).itcount(), "q3 a");

// We expect these queries to use index scans over { tags: 1 }.
assert(isIxscan(t.find(q1).explain().queryPlanner.winningPlan), "e1");
assert(isIxscan(t.find(q2).explain().queryPlanner.winningPlan), "e2");
assert(isIxscan(t.find(q3).explain().queryPlanner.winningPlan), "e3");

scanned1 = t.find(q1).explain("executionStats").executionStats.totalKeysExamined;
scanned2 = t.find(q2).explain("executionStats").executionStats.totalKeysExamined;
scanned3 = t.find(q3).explain("executionStats").executionStats.totalKeysExamined;

// print( "scanned1: " + scanned1 + " scanned2: " + scanned2 + " scanned3: " + scanned3 );

// $all should just iterate either of the words
assert(scanned3 <= Math.max(scanned1, scanned2), "$all makes query optimizer not work well");
