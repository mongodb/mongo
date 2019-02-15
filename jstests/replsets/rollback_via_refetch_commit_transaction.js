/**
 * Test that a node crashes if it tries to roll back a 'commit' oplog entry using refetch-based
 * rollback. The tests mimics the standard PSA rollback setup by using a PSS replica set where the
 * last node effectively acts as an arbiter without formally being one (this is necessary because
 * we disallow the 'prepareTransaction' command in sets with arbiters).
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

TestData.skipCheckDBHashes = true;

(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/libs/rollback_test.js");

    const dbName = "test";
    const collName = "rollback_via_refetch_commit_transaction";

    // Provide RollbackTest with custom ReplSetTest so we can set forceRollbackViaRefetch.
    const rst = new ReplSetTest({
        name: collName,
        nodes: 3,
        useBridge: true,
        nodeOptions: {setParameter: "forceRollbackViaRefetch=true"}
    });

    rst.startSet();
    const config = rst.getReplSetConfig();
    config.members[2].priority = 0;
    config.settings = {chainingAllowed: false};
    rst.initiate(config);

    const rollbackTest = new RollbackTest(collName, rst);

    // Create collection that exists on the sync source and rollback node.
    assert.commandWorked(rollbackTest.getPrimary().getDB(dbName).runCommand(
        {create: collName, writeConcern: {w: 3}}));

    // Stop replication from the current primary ("rollbackNode").
    const rollbackNode = rollbackTest.transitionToRollbackOperations();

    // Issue a 'prepareTransaction' command just to the current primary.
    const session = rollbackNode.getDB(dbName).getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({"prepare": "entry"}));
    const result = assert.commandWorked(
        session.getDatabase('admin').adminCommand({prepareTransaction: 1, writeConcern: {w: 1}}));
    assert(result.prepareTimestamp,
           "prepareTransaction did not return a 'prepareTimestamp': " + tojson(result));
    PrepareHelpers.commitTransactionAfterPrepareTS(session, result.prepareTimestamp);

    // Step down current primary and elect a node that lacks the commit.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // Verify the old primary crashes trying to roll back.
    clearRawMongoProgramOutput();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    jsTestLog("Waiting for crash");
    assert.soon(function() {
        try {
            rollbackNode.getDB("local").runCommand({ping: 1});
        } catch (e) {
            return true;
        }
        return false;
    }, "Node did not fassert", ReplSetTest.kDefaultTimeoutMS);

    // Let the ReplSetTest know the old primary is down.
    rst.stop(rst.getNodeId(rollbackNode), undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

    const msg = RegExp("Can't roll back this command yet: ");
    assert.soon(function() {
        return rawMongoProgramOutput().match(msg);
    }, "Node did not fail to roll back entry.");

    rst.stopSet();
}());
