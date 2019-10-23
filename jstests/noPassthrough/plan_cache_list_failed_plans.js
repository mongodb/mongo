// Confirms the planCacheListPlans output format includes information about failed plans.
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const testDB = conn.getDB("jstests_plan_cache_list_failed_plans");
const coll = testDB.test;

coll.drop();

// Setup the database such that it will generate a failing plan and a succeeding plan.
const numDocs = 32;
const smallNumber = 10;
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: smallNumber}));
for (let i = 0; i < numDocs * 2; ++i)
    assert.commandWorked(coll.insert({a: ((i >= (numDocs * 2) - smallNumber) ? 1 : 0), d: i}));

// Create the indexes to create competing plans.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({d: 1}));

// Assert that the find command found documents.
const key = {
    query: {a: 1},
    sort: {d: 1},
    projection: {}
};
assert.eq(smallNumber, coll.find(key.query).sort(key.sort).itcount());
let res = assert.commandWorked(coll.runCommand("planCacheListPlans", key));

// There should have been two plans generated.
assert.eq(res["plans"].length, 2);
// The second plan should fail.
assert.eq(res["plans"][1]["reason"]["failed"], true);

// The failing plan should have a score of 0.
assert.eq(res["plans"][1]["reason"]["score"], 0);
MongoRunner.stopMongod(conn);
})();
