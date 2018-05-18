/**
 * Test that transactions are executed speculatively.  This means two transactions affecting
 * the same document can run back to back without waiting for the first transaction to
 * commit to a majority.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    load("jstests/libs/write_concern_util.js");  // For stopServerReplication

    const dbName = "test";
    const collName = "speculative_transaction";

    const rst = new ReplSetTest({name: collName, nodes: [{}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    var testDB = primary.getDB(dbName);
    const coll = testDB[collName];

    // Do an initial write so we have something to update.
    assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}], {w: "majority"}));
    rst.awaitLastOpCommitted();

    // Stop replication on the secondary so the majority commit never moves forward.
    stopServerReplication(secondary);

    // Initiate a session on the primary.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    // Start the first transaction.  Do not use majority commit for this one.
    jsTestLog("Starting first transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: 1}});

    assert.commandWorked(sessionColl.update({_id: 0}, {$set: {x: 1}}));

    session.commitTransaction();

    // The document should be updated on the local snapshot.
    assert.eq(coll.findOne({_id: 0}), {_id: 0, x: 1});

    // The document should not be updated in the majority snapshot.
    assert.eq(coll.find({_id: 0}).readConcern("majority").next(), {_id: 0});

    jsTestLog("Starting second transaction");
    // Start a second transaction.  Still do not use majority commit for this one.
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: 1}});

    // We should see the updated doc within the transaction as a result of speculative read concern.
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 1});

    // Update it again.
    assert.commandWorked(sessionColl.update({_id: 0}, {$inc: {x: 1}}));

    // Update a different document outside the transaction.
    assert.commandWorked(coll.update({_id: 1}, {$set: {y: 1}}));

    // Within the transaction, we should not see the out-of-transaction update.
    assert.eq(sessionColl.findOne({_id: 1}), {_id: 1});

    session.commitTransaction();

    // The document should be updated on the local snapshot.
    assert.eq(coll.findOne({_id: 0}), {_id: 0, x: 2});

    // The document should not be updated in the majority snapshot.
    assert.eq(coll.find({_id: 0}).readConcern("majority").next(), {_id: 0});

    // Make sure write conflicts are caught with speculative transactions.
    jsTestLog("Starting a conflicting transaction which will be auto-aborted");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: 1}});

    // Read some data inside the transaction.
    assert.eq(sessionColl.findOne({_id: 1}), {_id: 1, y: 1});

    // Write it outside the transaction.
    assert.commandWorked(coll.update({_id: 1}, {$inc: {x: 1}}));

    // Can still read old data in transaction.
    assert.eq(sessionColl.findOne({_id: 1}), {_id: 1, y: 1});

    // But update fails
    assert.commandFailedWithCode(sessionColl.update({_id: 1}, {$inc: {x: 1}}),
                                 ErrorCodes.WriteConflict);

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Restart server replication to allow majority commit point to advance.
    restartServerReplication(secondary);

    jsTestLog("Starting final transaction (with majority commit)");
    // Start a third transaction, with majority commit.
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // We should see the updated doc within the transaction.
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 2});

    // Update it one more time.
    assert.commandWorked(sessionColl.update({_id: 0}, {$inc: {x: 1}}));

    session.commitTransaction();

    // The document should be updated on the local snapshot.
    assert.eq(coll.findOne({_id: 0}), {_id: 0, x: 3});

    // The document should also be updated in the majority snapshot.
    assert.eq(coll.find({_id: 0}).readConcern("majority").next(), {_id: 0, x: 3});

    session.endSession();

    rst.stopSet();
}());
