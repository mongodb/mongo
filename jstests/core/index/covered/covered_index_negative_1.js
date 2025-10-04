// Miscellaneous covered query tests. Mostly negative tests
// These are tests where we do not expect the query to be a
// covered index query. Hence we expect indexOnly=false and
// nscannedObjects > 0

// Include helpers for analyzing explain output.
// @tags: [
//   assumes_balancer_off,
//   does_not_support_stepdowns,
// ]
import {getWinningPlanFromExplain, isIndexOnly} from "jstests/libs/query/analyze_plan.js";

const coll = db.covered_negative_1;
coll.drop();
for (let i = 0; i < 100; i++) {
    assert.commandWorked(
        coll.insert({a: i, b: "strvar_" + (i % 13), c: NumberInt(i % 10), d: i * 10, e: [i, i % 10], f: i}),
    );
}
assert.commandWorked(coll.createIndex({a: 1, b: -1, c: 1}));
assert.commandWorked(coll.createIndex({e: 1}));
assert.commandWorked(coll.createIndex({d: 1}));
assert.commandWorked(coll.createIndex({f: "hashed"}));

// Test no projection
let plan = coll.find({a: 10, b: "strvar_10", c: 0}).hint({a: 1, b: -1, c: 1}).explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.1 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.1 - docs examined should not be 0 for a non covered query",
);

// Test projection and not excluding _id
plan = coll.find({a: 10, b: "strvar_10", c: 0}, {a: 1, b: 1, c: 1}).hint({a: 1, b: -1, c: 1}).explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.2 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.2 - docs examined should not be 0 for a non covered query",
);

// Test projection of non-indexed field
plan = coll.find({d: 100}, {d: 1, c: 1, _id: 0}).hint({d: 1}).explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.3 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.3 - docs examined should not be 0 for a non covered query",
);

// Test query and projection on a multi-key index
plan = coll.find({e: 99}, {e: 1, _id: 0}).hint({e: 1}).explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.4 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.4 - docs examined should not be 0 for a non covered query",
);

// Commenting out negative.1.5 and 1.6 pending fix in SERVER-8650
// // Test projection and $natural sort
// var plan = coll.find({a:{$gt:70}},{a:1, b:1, c:1, _id:0}).sort({$natural:1}).hint({a:1, b:-1,
// c:1}).explain()
// // indexOnly should be false but is not due to bug https://jira.mongodb.org/browse/SERVER-8561
// assert.eq(true, plan.indexOnly, "negative.1.5 - indexOnly should be false on a non covered
// query")
// assert.neq(0, plan.nscannedObjects, "negative.1.5 - nscannedObjects should not be 0 for a non
// covered query")

// // Test sort on non-indexed field
// var plan = coll.find({d:{$lt:1000}},{d:1, _id:0}).sort({c:1}).hint({d:1}).explain()
// //indexOnly should be false but is not due to bug https://jira.mongodb.org/browse/SERVER-8562
// assert.eq(true, plan.indexOnly, "negative.1.6 - indexOnly should be false on a non covered
// query")
// assert.neq(0, plan.nscannedObjects, "negative.1.6 - nscannedObjects should not be 0 for a non
// covered query")

// Test query on non-indexed field
plan = coll
    .find({d: {$lt: 1000}}, {a: 1, b: 1, c: 1, _id: 0})
    .hint({a: 1, b: -1, c: 1})
    .explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.7 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.7 - docs examined should not be 0 for a non covered query",
);

// Test query on hashed indexed field
plan = coll.find({f: 10}, {f: 1, _id: 0}).hint({f: "hashed"}).explain("executionStats");
assert(
    !isIndexOnly(db, getWinningPlanFromExplain(plan)),
    "negative.1.8 - indexOnly should be false on a non covered query",
);
assert.neq(
    0,
    plan.executionStats.totalDocsExamined,
    "negative.1.8 - nscannedObjects should not be 0 for a non covered query",
);

print("all tests passed");
