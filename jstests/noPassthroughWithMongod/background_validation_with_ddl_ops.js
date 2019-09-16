/**
 * Tests that an in-progress background validation gets interrupted when certain DDL operations are
 * executed.
 *
 * Dropping an index, the collection or database that are part of the ongoing background validation
 * should cause the validation to be interrupted. Renaming the collection across databases should
 * also interrupt, but same database collection renames should not. Operations that are not part of
 * the ongoing background validation, like creating new indexes or inserting documents should not
 * cause the validation to be interrupted.
 *
 * Only run this against WiredTiger, which supports checkpoint cursors.
 * @tags: [requires_wiredtiger, requires_persistence]
 */
(function() {
"use strict";
load("jstests/libs/check_log.js");

const dbName = "background_validation_with_ddl_ops";
const dbNameRename = "background_validation_with_ddl_ops_rename";
const collName = "test";

let testDb, testColl;

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

const resetCollection = () => {
    // Clear the log to get rid of any existing fail point logging that will be used to hang on.
    testDb = db.getSiblingDB(dbName);
    assert.commandWorked(testDb.adminCommand({clearLog: 'global'}));

    testColl = testDb.getCollection(collName);
    testColl.drop();
    db.getSiblingDB(dbNameRename).getCollection(collName).drop();

    assert.commandWorked(testColl.createIndex({x: 1}));

    // Insert 10,000 documents because validation will yield every 4096 entries fetched.
    const docsToInsert = 10000;
    var bulk = testColl.initializeUnorderedBulkOp();
    for (var i = 0; i < docsToInsert; i++) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());

    assert.commandWorked(testDb.fsyncLock());
    assert.commandWorked(testDb.fsyncUnlock());
};

/**
 * Collection validation fails due to dropped index that was being validated.
 */
resetCollection();

let awaitFailedValidationDueToIndexDrop;
try {
    setFailpoint();
    awaitFailedValidationDueToIndexDrop = startParallelShell(function() {
        assert.commandFailedWithCode(db.getSiblingDB("background_validation_with_ddl_ops")
                                         .runCommand({validate: "test", background: true}),
                                     ErrorCodes.Interrupted);
    });

    waitUntilFailpoint();
    assert.commandWorked(testColl.dropIndex({x: 1}));
} finally {
    unsetFailpoint();
}

awaitFailedValidationDueToIndexDrop();

/**
 * Collection validation fails due to dropped collection that was being validated.
 */
resetCollection();

let awaitFailedValidationDueToCollectionDrop;
try {
    setFailpoint();
    awaitFailedValidationDueToCollectionDrop = startParallelShell(function() {
        assert.commandFailedWithCode(db.getSiblingDB("background_validation_with_ddl_ops")
                                         .runCommand({validate: "test", background: true}),
                                     ErrorCodes.Interrupted);
    });

    waitUntilFailpoint();
    assert.eq(true, testColl.drop());
} finally {
    unsetFailpoint();
}

awaitFailedValidationDueToCollectionDrop();

/**
 * Collection validation fails due to being renamed across databases while being validated.
 */
resetCollection();

let awaitFailedValidationDueToCrossDBCollectionRename;
try {
    setFailpoint();
    awaitFailedValidationDueToCrossDBCollectionRename = startParallelShell(function() {
        assert.commandFailedWithCode(db.getSiblingDB("background_validation_with_ddl_ops")
                                         .runCommand({validate: "test", background: true}),
                                     ErrorCodes.Interrupted);
    });

    waitUntilFailpoint();
    assert.commandWorked(testDb.adminCommand({
        renameCollection: dbName + "." + collName,
        to: dbNameRename + "." + collName,
        dropTarget: true
    }));
} finally {
    unsetFailpoint();
}

awaitFailedValidationDueToCrossDBCollectionRename();

/**
 * Collection validation fails due to database being dropped.
 */
resetCollection();

let awaitFailedValidationDueToDatabaseDrop;
try {
    setFailpoint();
    awaitFailedValidationDueToDatabaseDrop = startParallelShell(function() {
        assert.commandFailedWithCode(db.getSiblingDB("background_validation_with_ddl_ops")
                                         .runCommand({validate: "test", background: true}),
                                     ErrorCodes.Interrupted);
    });

    waitUntilFailpoint();
    assert.commandWorked(testDb.dropDatabase());
} finally {
    unsetFailpoint();
}

awaitFailedValidationDueToDatabaseDrop();

/**
 * Collection validation succeeds when running operations that do not affect ongoing background
 * validation.
 */
resetCollection();

let awaitPassedValidation;
try {
    setFailpoint();
    awaitPassedValidation = startParallelShell(function() {
        assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                 .runCommand({validate: "test", background: true}));
    });

    waitUntilFailpoint();

    // Run a bunch of operations that shouldn't make background validation fail.
    assert.commandWorked(testColl.createIndex({y: 1}));
    assert.commandWorked(testColl.insert({x: 1, y: 1}));
    assert.commandWorked(testDb.createCollection("newCollection"));
    assert.commandWorked(testDb.adminCommand({
        renameCollection: dbName + "." +
            "newCollection",
        to: dbName + ".renamedCollection",
        dropTarget: true
    }));
    assert.commandWorked(testColl.dropIndex({y: 1}));

    // Rename the collection being validated across the same database.
    assert.commandWorked(testDb.adminCommand({
        renameCollection: dbName + "." + collName,
        to: dbName + ".testRenamed",
        dropTarget: true
    }));
} finally {
    unsetFailpoint();
}

awaitPassedValidation();
}());
