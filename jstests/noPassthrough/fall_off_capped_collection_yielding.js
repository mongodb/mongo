/**
 * Tests that falling off a capped collection across a yield results in the correct error.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod();
const testDB = conn.getDB('test');

const coll = testDB.fall_off_capped_collection_yielding;
const kCollectionMaxSize = 20;
coll.drop();
assert.commandWorked(
    testDB.createCollection(coll.getName(), {capped: true, size: 4096, max: kCollectionMaxSize}));

// Insert 10 documents.
const numDocs = 10;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}

// The classic query engine may check for interrupt/check for yield a couple times before accessing
// data, so 'internalQueryExecYieldIterations' must be set to a value slightly higher to ensure the
// classic query engine has accessed data before deciding to yield.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 5}));
// Set the yield MS counter to a very high value so that we never yield due to time.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 50000}));

const failPoint =
    configureFailPoint(testDB, "setYieldAllLocksHang", {namespace: coll.getFullName()});
let joinParallelShell = null;
// We use this try/finally pattern to ensure that the fail point gets disabled even if the test
// fails.
try {
    // In a separate shell, run the query.
    joinParallelShell = startParallelShell(function() {
        const err =
            assert.throws(() => printjson(db.fall_off_capped_collection_yielding.find().toArray()));
        assert.eq(err.code, ErrorCodes.CappedPositionLost);
    }, conn.port);

    failPoint.wait();

    // Now do a bunch of inserts, rolling over the capped collection.
    for (let i = 0; i < kCollectionMaxSize; ++i) {
        assert.commandWorked(coll.insert({_id: 100 + i}));
    }

} finally {
    // Unblock the thread doing the query by disabling the failpoint.
    failPoint.off();
}

// Join with the parallel shell.
if (joinParallelShell) {
    joinParallelShell();
}

MongoRunner.stopMongod(conn);
})();
