/**
 * Tests the validate command with {background:true} in a replica set.
 *
 * Checks that {full:true} cannot be run with {background:true}.
 * Checks that {background:true} runs.
 * Checks that {background:true} can run concurrently with CRUD ops on the same collection.
 *
 * @tags: [
 *   # Background validation is only supported by WT.
 *   requires_wiredtiger,
 *   # inMemory does not have checkpoints; background validation only runs on a checkpoint.
 *   requires_persistence,
 *   # A failpoint is set that only exists on the mongod.
 *   assumes_against_mongod_not_mongos,
 *   # A failpoint is set against the primary only.
 *   does_not_support_stepdowns,
 *   # Checkpoint cursors cannot be open in lsm.
 *   does_not_support_wiredtiger_lsm,
 *   # Background validation will be first available in v4.4.
 *   requires_fcv_44,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/core/txns/libs/prepare_helpers.js");

const dbName = "db_background_validation_repl";
const collName = "coll_background_validation_repl";

// Starts and returns a replica set.
const initTest = () => {
    const replSet = new ReplSetTest({nodes: 1, name: "rs"});
    replSet.startSet();
    replSet.initiate();
    const primary = replSet.getPrimary();

    let testColl = primary.getDB(dbName)[collName];
    testColl.drop();
    return replSet;
};

const doTest = replSet => {
    /*
     * Create some indexes and insert some data, so we can validate them more meaningfully.
     */
    const testDB = replSet.getPrimary().getDB(dbName);
    const testColl = testDB[collName];
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
                                 ErrorCodes.CommandNotSupported);

    assert.commandWorked(testDB.adminCommand({fsync: 1}));

    // Check that {backround:true} is successful.
    let res = testColl.validate({background: true});
    assert.commandWorked(res);
    assert(res.valid, "Validate cmd with {background:true} failed: " + tojson(res));

    /*
     * Test background validation with concurrent CRUD operations.
     */

    // Set a failpoint in the background validation code to pause validation while holding a
    // collection lock.
    let failPoint = configureFailPoint(testDB, "pauseCollectionValidationWithLock");

    jsTest.log(`Starting parallel shell on port ${replSet.getPrimary().port}`);
    // Start an asynchronous thread to run collection validation with {background:true}.
    // Ensure we can perform multiple collection validations on the same collection
    // concurrently.
    let awaitValidateCommand = startParallelShell(function() {
        const asyncTestDB = db.getSiblingDB("db_background_validation_repl");
        const asyncTestColl = asyncTestDB.coll_background_validation_repl;
        const validateRes = asyncTestColl.validate({background: true});
        assert.commandWorked(
            validateRes, "asynchronous background validate command failed: " + tojson(validateRes));
        assert(validateRes.valid,
               "asynchronous background validate command was not valid: " + tojson(validateRes));
    }, replSet.getPrimary().port);

    // Wait for background validation command to start.
    jsTest.log("Waiting for failpoint to hit...");
    failPoint.wait();

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
    assert.eq(res.keysPerIndex._id_,
              numDocs,
              "Expected " + numDocs + " _id index records: " + tojson(res));
    assert.eq(res.keysPerIndex.a_1,
              numDocs,
              "Expected " + numDocs + " a_1 index records: " + tojson(res));
    assert.eq(res.keysPerIndex.b_1,
              numDocs,
              "Expected " + numDocs + " b_1 index records: " + tojson(res));
    assert.eq(res.keysPerIndex.c_1,
              numDocs,
              "Expected " + numDocs + " c_1 index records: " + tojson(res));
};

const replSet = initTest();
doTest(replSet);
replSet.stopSet();
})();
