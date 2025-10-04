// @tags: [
//   assumes_balancer_off,
//   requires_getmore
// ]

let t = db.index_check2;
t.drop();

// Include helpers for analyzing explain output.
import {getWinningPlanFromExplain, isIxscan} from "jstests/libs/query/analyze_plan.js";

for (let i = 0; i < 1000; i++) {
    let a = [];
    for (let j = 1; j < 5; j++) {
        a.push("tag" + ((i * j) % 50));
    }
    t.save({num: i, tags: a});
}

let q1 = {tags: "tag6"};
let q2 = {tags: "tag12"};
let q3 = {tags: {$all: ["tag6", "tag12"]}};

assert.eq(120, t.find(q1).itcount(), "q1 a");
assert.eq(120, t.find(q2).itcount(), "q2 a");
assert.eq(60, t.find(q3).itcount(), "q3 a");

t.createIndex({tags: 1});

assert.eq(120, t.find(q1).itcount(), "q1 a");
assert.eq(120, t.find(q2).itcount(), "q2 a");
assert.eq(60, t.find(q3).itcount(), "q3 a");

// We expect these queries to use index scans over { tags: 1 }.
assert(isIxscan(db, getWinningPlanFromExplain(t.find(q1).explain())), "e1");
assert(isIxscan(db, getWinningPlanFromExplain(t.find(q2).explain())), "e2");
assert(isIxscan(db, getWinningPlanFromExplain(t.find(q3).explain())), "e3");

let scanned1 = t.find(q1).explain("executionStats").executionStats.totalKeysExamined;
let scanned2 = t.find(q2).explain("executionStats").executionStats.totalKeysExamined;
let scanned3 = t.find(q3).explain("executionStats").executionStats.totalKeysExamined;
// $all should just iterate either of the words
assert(scanned3 <= Math.max(scanned1, scanned2), "$all makes query optimizer not work well");
