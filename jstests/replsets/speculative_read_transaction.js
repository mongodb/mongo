/**
 * Test that read-only transactions are executed speculatively, and that once they are committed
 * with a majority write concern, the data read is indeed majority-committed.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    load("jstests/libs/write_concern_util.js");  // For stopServerReplication

    const dbName = "test";
    const collName = "speculative_read_transaction";

    const rst = new ReplSetTest({name: collName, nodes: [{}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const testDB = primary.getDB(dbName);
    const coll = testDB[collName];

    // Do an initial write so we have something to update.
    assert.commandWorked(coll.insert([{_id: 0, x: 0}], {w: "majority"}));
    rst.awaitLastOpCommitted();

    // Stop replication on the secondary so the majority commit never moves forward.
    stopServerReplication(secondary);

    // Do a local update in another client.
    // The transaction should see this, due to speculative behavior.
    const otherclient = new Mongo(primary.host);
    assert.commandWorked(otherclient.getDB(dbName)[collName].update({_id: 0}, {x: 1}, {w: 1}));

    // Initiate a session on the primary.
    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    // Abort does not wait for write concern.
    jsTestLog("Starting majority-abort transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 1});
    assert.commandWorked(session.abortTransaction_forTesting());

    // This transaction should complete because it does not use majority write concern.
    jsTestLog("Starting non-majority commit transaction");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: 1}});
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 1});
    session.commitTransaction();

    // This transaction should not complete because it uses snapshot read concern, majority
    // write concern and the commit point is not advancing.
    jsTestLog("Starting majority-commit snapshot-read transaction");
    session.startTransaction(
        {readConcern: {level: "snapshot"}, writeConcern: {w: "majority", wtimeout: 5000}});
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 1});
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.WriteConcernFailed);

    // Allow the majority commit point to advance to allow the failed write concern to clear.
    restartServerReplication(secondary);
    rst.awaitReplication();
    stopServerReplication(secondary);

    // Do another local update from another client
    assert.commandWorked(otherclient.getDB(dbName)[collName].update({_id: 0}, {x: 2}, {w: 1}));

    // This transaction should not complete because it uses local read concern upconverted to
    // snapshot.
    // TODO(SERVER-34881): Once default read concern is speculative majority, local read
    //                     concern should not wait for the majority commit point to advance.
    jsTestLog("Starting majority-commit local-read transaction");
    session.startTransaction(
        {readConcern: {level: "local"}, writeConcern: {w: "majority", wtimeout: 5000}});
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 2});
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.WriteConcernFailed);

    // Allow the majority commit point to advance to allow the failed write concern to clear.
    restartServerReplication(secondary);
    rst.awaitReplication();
    stopServerReplication(secondary);

    // Do another local update from another client
    assert.commandWorked(otherclient.getDB(dbName)[collName].update({_id: 0}, {x: 3}, {w: 1}));

    // This transaction should not complete because it uses majority read concern, majority write
    // concern, and the commit point is not advancing.
    jsTestLog("Starting majority-commit majority-read transaction");
    session.startTransaction(
        {readConcern: {level: "majority"}, writeConcern: {w: "majority", wtimeout: 5000}});
    assert.eq(sessionColl.findOne({_id: 0}), {_id: 0, x: 3});
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.WriteConcernFailed);

    // Restart server replication to allow majority commit point to advance.
    restartServerReplication(secondary);

    session.endSession();

    rst.stopSet();
}());
