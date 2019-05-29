/**
 * Tests that using the "failNonIntentLocksIfWaitNeeded" failpoint allows us to immediately
 * fail DDL operations blocked behind prepare, as we know they will not be able to acquire locks
 * anyway. The DDL ops will fail here because they won't be able to get a MODE_X lock on the
 * global or database resources.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/libs/get_index_helpers.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "ddl_ops_after_prepare_lock_failpoint";
    const indexName = "test_index";

    const primary = rst.getPrimary();
    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    // Create the collection we will be working with.
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Also build an index (on the same collection) which we will later attempt to drop.
    assert.commandWorked(testDB.runCommand(
        {createIndexes: collName, indexes: [{key: {"num": 1}, name: indexName}]}));

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 42}));

    PrepareHelpers.prepareTransaction(session);

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "alwaysOn"}));

    /**
     * Tests that conflicting DDL ops fail immediately.
     */

    // Collection names for DDL ops that will fail.
    const collToDrop = collName;
    const collToRenameFrom = collName;
    const collToRenameTo = "rename_collection_to_fail";
    const indexToCreate = "create_index_to_fail";
    const indexToDrop = indexName;

    let testDDLOps = () => {
        // Also attempt to delete our original collection (it is in conflict anyway, but should
        // fail to acquire the db lock in the first place).
        assert.throws(function() {
            testDB.getCollection(collToDrop).drop();
        });
        assert(testDB.getCollectionNames().includes(collToDrop));

        // Same goes for trying to rename it.
        assert.commandFailedWithCode(
            testDB.getCollection(collToRenameFrom).renameCollection(collToRenameTo),
            ErrorCodes.LockTimeout);
        assert(testDB.getCollectionNames().includes(collToRenameFrom));
        assert(!testDB.getCollectionNames().includes(collToRenameTo));

        assert.commandFailedWithCode(testDB.adminCommand({
            renameCollection: testDB.getCollection(collToRenameFrom).getFullName(),
            to: testDB.getSiblingDB('test2').getCollection(collToRenameTo).getFullName(),
        }),
                                     ErrorCodes.LockTimeout);

        // Attempt to add a new index to that collection.
        assert.commandFailedWithCode(
            testDB.runCommand(
                {createIndexes: collName, indexes: [{key: {"b": 1}, name: indexToCreate}]}),
            ErrorCodes.LockTimeout);
        assert.eq(null, GetIndexHelpers.findByName(testColl.getIndexes(), indexToCreate));

        // Try dropping the index we created originally. This should also fail.
        assert.commandFailedWithCode(testDB.runCommand({dropIndexes: collName, index: indexToDrop}),
                                     ErrorCodes.LockTimeout);
        assert.neq(null, GetIndexHelpers.findByName(testColl.getIndexes(), indexToDrop));
    };

    /**
     * Tests that CRUD operations on the same collection succeed.
     */

    const docToInsert = {num: 100};
    const docToUpdateFrom = docToInsert;
    const docToUpdateTo = {num: 101};
    const docToRemove = docToUpdateTo;

    let testCRUDOps = (collConn) => {
        // TODO: SERVER-40167 Having an extra document in the collection is necessary to avoid
        // prepare conflicts when deleting documents.
        assert.commandWorked(collConn.insert({num: 1}));

        assert.commandWorked(collConn.insert(docToInsert));
        assert.eq(100, collConn.findOne(docToInsert).num);

        // This will not encounter a prepare conflict because there is an index on "num" that
        // eliminates the need for using a collection scan.
        assert.commandWorked(collConn.update(docToUpdateFrom, docToUpdateTo));
        assert.eq(101, collConn.findOne(docToUpdateTo).num);

        assert.commandWorked(collConn.remove(docToRemove));
        assert.eq(null, collConn.findOne(docToUpdateFrom));
        assert.eq(null, collConn.findOne(docToUpdateTo));
    };

    // First test DDL ops (should fail).
    testDDLOps();

    // Then test operations outside of transactions (should succeed).
    testCRUDOps(testColl);

    // Also test operations as part of a transaction (should succeed).
    testCRUDOps(primary.startSession({causalConsistency: false})
                    .getDatabase(dbName)
                    .getCollection(collName));

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));

    assert.commandWorked(session.abortTransaction_forTesting());
    rst.stopSet();
})();
