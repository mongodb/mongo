/**
 * Verifies that background validation is interrupted when the `restartCatalog` command is
 * executed.
 *
 * Only run this against WiredTiger, which supports checkpoint cursors.
 * @tags: [requires_wiredtiger, requires_persistence]
 */
(function() {
"use strict";
load("jstests/libs/check_log.js");

const dbName = "restart_catalog_interrupts_background_validation";
const collName = "test";

let testDb = db.getSiblingDB(dbName);
let testColl = testDb.getCollection(collName);
testColl.drop();

const setFailpoint = () => {
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "hangDuringYieldingLocksForValidation", mode: "alwaysOn"}));
};

const unsetFailpoint = () => {
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "hangDuringYieldingLocksForValidation", mode: "off"}));
};

const waitUntilFailpoint = () => {
    checkLog.contains(testDb.getMongo(),
                      "Hanging on fail point 'hangDuringYieldingLocksForValidation'");
};

const setupCollection = () => {
    // Clear the log to get rid of any existing fail point logging that will be used to hang on.
    assert.commandWorked(testDb.adminCommand({clearLog: 'global'}));

    assert.commandWorked(testColl.createIndex({x: 1}));

    // Insert 10,000 documents because validation will yield every 4096 entries fetched.
    const docsToInsert = 10000;
    var bulk = testColl.initializeUnorderedBulkOp();
    for (var i = 0; i < docsToInsert; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());

    // Create a checkpoint of the data.
    assert.commandWorked(testDb.fsyncLock());
    assert.commandWorked(testDb.fsyncUnlock());
};

// Create an index, insert some test data and force a checkpoint.
setupCollection();

let awaitBackgroundValidationFailed;
try {
    setFailpoint();
    awaitBackgroundValidationFailed = startParallelShell(function() {
        assert.commandFailedWithCode(
            db.getSiblingDB("restart_catalog_interrupts_background_validation")
                .runCommand({validate: "test", background: true}),
            ErrorCodes.Interrupted);
    });

    waitUntilFailpoint();
    assert.commandWorked(db.adminCommand({restartCatalog: 1}));
} finally {
    unsetFailpoint();
}

awaitBackgroundValidationFailed();
}());
