/**
 * Tests that the following FCV constraints are observed when building indexes:
 *
 *   - Multiple indexes which differ only by partial filter expression can be built in FCV 4.6.
 *   - The planner can continue to use these indexes after downgrading to FCV 4.4.
 *   - These indexes can be dropped in FCV 4.4.
 *   - Indexes which differ only by partialFilterExpression cannot be created in FCV 4.4.
 *   - We do not fassert if the set is downgraded to binary 4.4 with "duplicate" indexes present.
 *
 * TODO SERVER-47766: this test is specific to the 4.4 - 4.6 upgrade process, and can be removed
 * after we branch for 4.7.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");               // For isIxscan and hasRejectedPlans.
load("jstests/multiVersion/libs/multi_rs.js");      // For upgradeSet.
load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {binVersion: "latest"},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB(jsTestName());
let coll = testDB.test;
coll.insert({a: 100});

// Verifies that the given query is indexed, and that 'numAlternativePlans' were generated.
function assertIndexedQuery(query, numAlternativePlans) {
    const explainOut = coll.explain().find(query).finish();
    assert(isIxscan(testDB, explainOut), explainOut);
    assert.eq(getRejectedPlans(explainOut).length, numAlternativePlans, explainOut);
}

// Test that multiple indexes differing only by partialFilterExpression can be created in FCV 4.6.
testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index1", partialFilterExpression: {a: {$gte: 0}}}));
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}));
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index3", partialFilterExpression: {a: {$gte: 100}}}));

// Test that the planner considers all relevant partial indexes when answering a query in FCV 4.6.
assertIndexedQuery({a: 1}, 0);
assertIndexedQuery({a: 11}, 1);
assertIndexedQuery({a: 101}, 2);

// Test that an index build restarted during startup recovery in FCV 4.6 does not revert to FCV 4.4
// behavior.
jsTestLog("Starting index build on primary and pausing before completion");
IndexBuildTest.pauseIndexBuilds(primary);
IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {name: "index4", partialFilterExpression: {a: {$lt: 0}}});

jsTestLog("Waiting for secondary to start the index build");
let secondary = rst.getSecondary();
let secondaryDB = secondary.getDB(jsTestName());
IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
rst.restart(secondary.nodeId);

jsTestLog("Waiting for all nodes to finish building the index");
IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.waitForIndexBuildToStop(testDB, coll.getName(), "index4");
rst.awaitReplication();

// Reset connection in case leadership has changed.
primary = rst.getPrimary();
testDB = primary.getDB(jsTestName());
coll = testDB.test;

assertIndexedQuery({a: -1}, 0);

// Test that these indexes are retained and can be used by the planner when we downgrade to FCV 4.4.
testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});
assertIndexedQuery({a: 1}, 0);
assertIndexedQuery({a: 11}, 1);
assertIndexedQuery({a: 101}, 2);

// Test that indexes distinguished only by partial filter can be dropped by name in FCV 4.4.
assert.commandWorked(coll.dropIndex("index2"));
assertIndexedQuery({a: 1}, 0);
assertIndexedQuery({a: 11}, 0);
assertIndexedQuery({a: 101}, 1);

// Test that an index distinguished only by partialFilterExpression cannot be created in FCV 4.4.
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}),
    ErrorCodes.IndexOptionsConflict);

// Test that attempting to build an index with the same name and identical partialFilterExpression
// as an existing index results in a no-op in FCV 4.4, and the command reports successful execution.
const cmdRes = assert.commandWorked(
    coll.createIndex({a: 1}, {name: "index1", partialFilterExpression: {a: {$gte: 0}}}));
assert.eq(cmdRes.numIndexesBefore, cmdRes.numIndexesAfter);

// Test that downgrading to binary 4.4 with overlapping partial indexes present does not fassert.
rst.upgradeSet({binVersion: "last-lts"});
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.test;

// Test that the indexes still exist and can be used to answer queries on the binary 4.4 replset.
assertIndexedQuery({a: 1}, 0);
assertIndexedQuery({a: 11}, 0);
assertIndexedQuery({a: 101}, 1);

// Test that an index which differs only by partialFilterExpression cannot be created on binary 4.4.
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {name: "index2", partialFilterExpression: {a: {$gte: 10}}}),
    ErrorCodes.IndexOptionsConflict);

rst.stopSet();
})();
