// Test that explain correctly outputs whether the planner hit or, and, or scan limits.

import {getOptimizer} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

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
switch (getOptimizer(orResult)) {
    case "classic":
        assert(orResult.queryPlanner.maxIndexedOrSolutionsReached, tojson(orResult));
        break;
    case "CQF":
        // TODO SERVER-77719: Implement the assertion for CQF.
        break;
}
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
switch (getOptimizer(andResult)) {
    case "classic":
        assert(andResult.queryPlanner.maxIndexedAndSolutionsReached, tojson(andResult));
        break;
    case "CQF":
        // TODO SERVER-77719: Implement the assertion for CQF.
        break;
}

// Test that andLimit and orLimit will both show in one query.
FixtureHelpers.runCommandOnEachPrimary({
    db: testDB.getSiblingDB("admin"),
    cmdObj: {
        setParameter: 1,
        "internalQueryEnumerationMaxOrSolutions": 1,
    }
});
const comboResult = coll.find({common: 1, one: 10, $or: [{one: 1}, {two: 2}]}).explain();
switch (getOptimizer(andResult)) {
    case "classic":
        assert(comboResult.queryPlanner.maxIndexedAndSolutionsReached, tojson(comboResult));
        assert(comboResult.queryPlanner.maxIndexedOrSolutionsReached, tojson(comboResult));
        break;
    case "CQF":
        // TODO SERVER-77719: Implement the assertion for CQF.
        break;
}

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
