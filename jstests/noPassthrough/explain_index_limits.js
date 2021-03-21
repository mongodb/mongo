// Test that explain correctly outputs whether the planner hit or, and, or scan limits.

(function() {
"use strict";
load("jstests/libs/fixture_helpers.js");

const conn = MongoRunner.runMongod({});
const testDB = conn.getDB(jsTestName());
const coll = testDB.planner_index_limit;
coll.drop();

// Test scanLimit.
coll.createIndex({e: 1, s: 1});
let inList = [];
for (let i = 0; i < 250; i++) {
    inList.push(i);
}
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryMaxScansToExplode": 0,
    }
});
const scanResult = coll.find({e: {$in: inList}}).sort({s: 1}).explain();
assert(scanResult.queryPlanner.maxScansToExplodeReached, tojson(scanResult));
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryMaxScansToExplode": 200,
    }
});
coll.drop();

// Test orLimit.
coll.createIndex({common: 1});
coll.createIndex({one: 1});
coll.createIndex({two: 1});
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxOrSolutions": 1,
    }
});
const orResult = coll.find({common: 1, $or: [{one: 0, two: 0}, {one: 1, two: 1}]}).explain();
assert(orResult.queryPlanner.maxIndexedOrSolutionsReached, tojson(orResult));
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxOrSolutions": 10,
    }
});

// Test andLimit.
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxIntersectPerAnd": 1,
    }
});
const andResult = coll.find({common: 1, two: 0, one: 1}).explain();
assert(andResult.queryPlanner.maxIndexedAndSolutionsReached, tojson(andResult));

// Test that andLimit and orLimit will both show in one query.
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxOrSolutions": 1,
    }
});
const comboResult = coll.find({common: 1, one: 10, $or: [{one: 1}, {two: 2}]}).explain();
assert(comboResult.queryPlanner.maxIndexedAndSolutionsReached, tojson(comboResult));
assert(comboResult.queryPlanner.maxIndexedOrSolutionsReached, tojson(comboResult));

// Reset values to defaults.
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxOrSolutions": 10,
    }
});
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxIntersectPerAnd": 3,
    }
});
MongoRunner.stopMongod(conn);
})();
