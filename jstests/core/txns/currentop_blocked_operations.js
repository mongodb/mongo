/**
 * Tests that currentOp reports debug information for operations that are blocked on transactions.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "currentop_blocked_operations";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    // Returns when the operation matching the 'matchExpr' is blocked, as evaluated by the
    // 'isBlockedFunc'.
    let waitForBlockedOp = function(matchExpr, isBlockedFunc) {
        assert.soon(function() {
            let cursor =
                db.getSiblingDB("admin").aggregate([{$currentOp: {}}, {$match: matchExpr}]);
            if (cursor.hasNext()) {
                let op = cursor.next();
                printjson(op);
                return isBlockedFunc(op);
            }
            return false;
        });
    };

    // This transaction will block conflicting non-transactional operations.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: 2222}));

    // This insert operation will encounter a WriteConflictException due to the unique key
    // violation. It will block in an infinite write conflict loop until the transaction completes.
    TestData.dbName = dbName;
    TestData.collName = collName;
    let awaitInsert = startParallelShell(function() {
        let coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
        assert.commandWorked(coll.insert({_id: 2222, x: 0}));
    });

    // Wait for the counter to reach a high enough number to confirm the operation is retrying
    // constantly.
    waitForBlockedOp({"command.insert": collName}, function(op) {
        return op.writeConflicts > 20;
    });

    assert.commandWorked(session.abortTransaction_forTesting());
    awaitInsert();
    assert.eq(1, testColl.find({_id: 2222, x: 0}).itcount());

    // This prepared transaction will block conflicting non-transactional operations.
    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 2222}, {$set: {x: 1}}));
    PrepareHelpers.prepareTransaction(session);

    // This update operation will encounter a prepare conflict due to the prepared transaction's
    // modification to the same document. It will block without retrying until the prepared
    // transaction completes.
    TestData.dbName = dbName;
    TestData.collName = collName;
    let awaitUpdate = startParallelShell(function() {
        let coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
        assert.commandWorked(coll.update({_id: 2222}, {$set: {x: 999}}));
    });

    // Expect at least one prepare conflict.
    waitForBlockedOp({ns: testColl.getFullName(), op: "update"}, function(op) {
        return op.prepareReadConflicts > 0;
    });

    assert.commandWorked(session.abortTransaction_forTesting());
    awaitUpdate();
    assert.eq(1, testColl.find({_id: 2222, x: 999}).itcount());
})();
