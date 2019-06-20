/**
 * Tests that a failing commitTransaction of a prepared transaction doesn't block stepdown. This is
 * a regression test for SERVER-41838.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";

    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "foo";

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();

    const config = rst.getReplSetConfig();
    // Increase the election timeout so that we do not accidentally trigger an election while
    // stepping up the old secondary.
    config.settings = {"electionTimeoutMillis": 12 * 60 * 60 * 1000};
    rst.initiate(config);

    const priConn = rst.getPrimary();
    const secConn = rst.getSecondary();
    assert.commandWorked(priConn.getDB(dbName).runCommand({create: collName}));

    const priSession = priConn.startSession();
    const priSessionDB = priSession.getDatabase(dbName);
    const priSessionColl = priSessionDB.getCollection(collName);

    jsTestLog("Prepare a transaction");
    priSession.startTransaction();
    assert.commandWorked(priSessionColl.insert({_id: 1}));
    const prepareTimestamp1 = PrepareHelpers.prepareTransaction(priSession);

    jsTestLog("Error committing the transaction");
    // This will error in the "commit unprepared transaction" code path.
    assert.commandFailedWithCode(priSessionDB.adminCommand({commitTransaction: 1}),
                                 ErrorCodes.InvalidOptions);

    // This will error in the "commit prepared transaction" code path.
    const tooEarlyTS1 = Timestamp(prepareTimestamp1.getTime() - 1, 1);
    assert.commandFailedWithCode(
        priSessionDB.adminCommand({commitTransaction: 1, commitTimestamp: tooEarlyTS1}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Step up the secondary");
    rst.stepUp(secConn);
    assert.eq(secConn, rst.getPrimary());
    rst.waitForState(priConn, ReplSetTest.State.SECONDARY);

    jsTestLog("commitTransaction command is retryable after failover");

    const secSession = new _DelegatingDriverSession(secConn, priSession);
    const secSessionDB = secSession.getDatabase(dbName);
    const secSessionColl = secSessionDB.getCollection(collName);
    assert.commandWorked(PrepareHelpers.commitTransaction(secSession, prepareTimestamp1));

    assert.eq(secConn.getDB(dbName)[collName].count(), 1);
    assert.eq(secConn.getDB(dbName)[collName].find().itcount(), 1);

    rst.awaitReplication();

    assert.eq(priConn.getDB(dbName)[collName].count(), 1);
    assert.eq(priConn.getDB(dbName)[collName].find().itcount(), 1);

    jsTestLog("Prepare a second transaction");
    secSession.startTransaction();
    assert.commandWorked(secSessionColl.insert({_id: 2}));
    const prepareTimestamp2 = PrepareHelpers.prepareTransaction(secSession);

    jsTestLog("Error committing the transaction");
    assert.commandFailedWithCode(secSessionDB.adminCommand({commitTransaction: 1}),
                                 ErrorCodes.InvalidOptions);
    const tooEarlyTS2 = Timestamp(prepareTimestamp2.getTime() - 1, 1);
    assert.commandFailedWithCode(
        secSessionDB.adminCommand({commitTransaction: 1, commitTimestamp: tooEarlyTS2}),
        ErrorCodes.InvalidOptions);

    jsTestLog("Step up the original primary");
    rst.stepUp(priConn);
    assert.eq(priConn, rst.getPrimary());
    rst.waitForState(secConn, ReplSetTest.State.SECONDARY);

    jsTestLog("Step up the original secondary immediately");
    rst.stepUp(secConn);
    assert.eq(secConn, rst.getPrimary());
    rst.waitForState(priConn, ReplSetTest.State.SECONDARY);

    assert.commandWorked(PrepareHelpers.commitTransaction(secSession, prepareTimestamp2));

    assert.eq(secConn.getDB(dbName)[collName].count(), 2);
    assert.eq(secConn.getDB(dbName)[collName].find().itcount(), 2);

    rst.awaitReplication();

    assert.eq(priConn.getDB(dbName)[collName].count(), 2);
    assert.eq(priConn.getDB(dbName)[collName].find().itcount(), 2);

    rst.stopSet();
}());
