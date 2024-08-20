/**
 * Tests that an in-progress background validation succeeds when DDL operations are executed against
 * the collection.
 *
 * @tags: [requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "background_validation_with_ddl_ops";
const dbNameRename = "background_validation_with_ddl_ops_rename";
const collName = "test";

let testDb, testColl;

const setFailpoint = () => {
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "hangDuringValidationInitialization", mode: "alwaysOn"}));
};

const unsetFailpoint = () => {
    assert.commandWorked(testDb.adminCommand(
        {configureFailPoint: "hangDuringValidationInitialization", mode: "off"}));
};

const waitUntilFailpoint = () => {
    checkLog.contains(testDb.getMongo(),
                      "Hanging on fail point 'hangDuringValidationInitialization'");
};

const resetCollection = () => {
    // Clear the log to get rid of any existing fail point logging that will be used to hang on.
    testDb = primary.getDB(dbName);
    assert.commandWorked(testDb.adminCommand({clearLog: 'global'}));

    testColl = testDb.getCollection(collName);
    testColl.drop();
    primary.getDB(dbNameRename).getCollection(collName).drop();

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
 * Collection validation succeeds when dropping an index being validated.
 */
resetCollection();

let awaitValidation;
try {
    setFailpoint();
    awaitValidation = startParallelShell(function() {
        let res = assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                           .runCommand({validate: "test", background: true}));
        assert(res.valid);
    }, primary.port);

    waitUntilFailpoint();
    assert.commandWorked(testColl.dropIndex({x: 1}));
} finally {
    unsetFailpoint();
}

awaitValidation();

/**
 * Collection validation succeeds when dropping the collection being validated.
 */
resetCollection();

try {
    setFailpoint();
    awaitValidation = startParallelShell(function() {
        let res = assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                           .runCommand({validate: "test", background: true}));
        assert(res.valid);
    }, primary.port);

    waitUntilFailpoint();
    assert.eq(true, testColl.drop());
} finally {
    unsetFailpoint();
}

awaitValidation();

/**
 * Collection validation succeeds when the collection being validated is renamed.
 */
resetCollection();

try {
    setFailpoint();
    awaitValidation = startParallelShell(function() {
        let res = assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                           .runCommand({validate: "test", background: true}));
        assert(res.valid);
    }, primary.port);

    waitUntilFailpoint();
    assert.commandWorked(testDb.adminCommand({
        renameCollection: dbName + "." + collName,
        to: dbNameRename + "." + collName,
        dropTarget: true
    }));
} finally {
    unsetFailpoint();
}

awaitValidation();

/**
 * Collection validation succeeds when dropping the database the collection being validated is part
 * of.
 */
resetCollection();

try {
    setFailpoint();
    awaitValidation = startParallelShell(function() {
        let res = assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                           .runCommand({validate: "test", background: true}));
        assert(res.valid);
    }, primary.port);

    waitUntilFailpoint();
    assert.commandWorked(testDb.dropDatabase());
} finally {
    unsetFailpoint();
}

awaitValidation();

/**
 * Collection validation succeeds when running operations that do not affect ongoing background
 * validation.
 */
resetCollection();

try {
    setFailpoint();
    awaitValidation = startParallelShell(function() {
        assert.commandWorked(db.getSiblingDB("background_validation_with_ddl_ops")
                                 .runCommand({validate: "test", background: true}));
    }, primary.port);

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

awaitValidation();

rst.stopSet();
