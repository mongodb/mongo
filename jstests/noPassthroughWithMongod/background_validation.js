/**
 * Tests the validate command with {background:true}.
 *
 * Checks that {full:true} cannot be run with {background:true}.
 * Checks that {background:true} runs.
 * Checks that {background:true} can run concurrently with CRUD ops on the same collection.
 *
 * @tags: [requires_fsync, requires_wiredtiger, requires_persistence]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const forceCheckpoint = () => {
    assert.commandWorked(db.fsyncLock());
    assert.commandWorked(db.fsyncUnlock());
};

const dbName = "test_db_background_validation";
const collName = "test_coll_background_validation";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);
testColl.drop();

/*
 * Create some indexes and insert some data, so we can validate them more meaningfully.
 */

assert.commandWorked(testColl.createIndex({a: 1}));
assert.commandWorked(testColl.createIndex({b: 1}));
assert.commandWorked(testColl.createIndex({c: 1}));

const numDocs = 100;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(testColl.insert({a: i, b: i, c: i}));
}

/**
 * Ensure {full:true} and {background:true} cannot be run together.
 */
assert.commandFailedWithCode(testColl.validate({background: true, full: true}),
                             ErrorCodes.InvalidOptions);

forceCheckpoint();

// Check that {backround:true} is successful.
let res = testColl.validate({background: true});
assert.commandWorked(res);
assert(res.valid, "Validate cmd with {background:true} failed: " + tojson(res));

/*
 * Test background validation with concurrent CRUD operations.
 */

// Set a failpoint in the background validation code to pause validation while holding a collection
// lock.
let failPoint = configureFailPoint(db, "pauseCollectionValidationWithLock");

// Start an asynchronous thread to run collection validation with {background:true}.
let awaitValidateCommand = startParallelShell(function() {
    let mycoll = db.getSiblingDB("test_db_background_validation")
                     .getCollection("test_coll_background_validation");
    let validateRes = mycoll.validate({background: true});
    assert.commandWorked(validateRes,
                         "asynchronous background validate command failed: " + tojson(validateRes));
    assert(validateRes.valid,
           "asynchronous background validate command was not valid: " + tojson(validateRes));
});

// Wait for background validation command to start.
failPoint.wait();

jsTest.log("Should start hanging now......");

// Check that CRUD ops are succesful while validation is in progress.
assert.commandWorked(testColl.remove({a: 1, b: 1, c: 1}));
assert.commandWorked(testColl.insert({a: 1, b: 1, c: 1, d: 100}));
assert.commandWorked(testColl.update({d: 100}, {"e": "updated"}));
let docRes = testColl.find({"e": "updated"});
assert.eq(1,
          docRes.toArray().length,
          "expected to find a single document, found: " + tojson(docRes.toArray()));

// Clear the failpoint and make sure the validate command was successful.
failPoint.off();
awaitValidateCommand();

/**
 * Verify everything is still OK by running foreground validation.
 */
res = testColl.validate({background: false});
assert.commandWorked(res);
assert(res.valid, "Validate cmd with {background:true} failed: " + tojson(res));
assert.eq(res.nIndexes, 4, "Expected 4 indexes: " + tojson(res));
assert.eq(res.nrecords, numDocs, "Expected " + numDocs + " collection records:" + tojson(res));
assert.eq(
    res.keysPerIndex._id_, numDocs, "Expected " + numDocs + " _id index records: " + tojson(res));
assert.eq(
    res.keysPerIndex.a_1, numDocs, "Expected " + numDocs + " a_1 index records: " + tojson(res));
assert.eq(
    res.keysPerIndex.b_1, numDocs, "Expected " + numDocs + " b_1 index records: " + tojson(res));
assert.eq(
    res.keysPerIndex.c_1, numDocs, "Expected " + numDocs + " c_1 index records: " + tojson(res));
})();
