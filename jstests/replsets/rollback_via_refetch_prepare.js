/**
 * Test that a node crashes if it tries to roll back a 'prepare' oplog entry using refetch-based
 * rollback. The tests mimics the standard PSA rollback setup by using a PSS replica set where the
 * last node effectively acts as an arbiter without formally being one (this is necessary because
 * we disallow the 'prepareTransaction' command in sets with arbiters).
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
    "use strict";
    load("jstests/libs/write_concern_util.js");
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = "rollback_via_refetch_prepare";

    const rst = new ReplSetTest({
        name: collName,
        nodes: 3,
        useBridge: true,
        nodeOptions: {setParameter: "forceRollbackViaRefetch=true"}
    });
    let nodes = rst.startSet();

    let config = rst.getReplSetConfig();
    config.members[2].priority = 0;
    rst.initiate(config);

    const rollbackNode = nodes[0];
    const syncSource = nodes[1];
    const tiebreakerNode = nodes[2];

    // Wait for primary to be up and ready.
    let primary = rst.getPrimary();
    assert.eq(rollbackNode, primary);

    // Also wait for the secondaries.
    rst.awaitSecondaryNodes();

    // Create the collection we're using beforehand.
    assert.commandWorked(
        primary.getDB(dbName).runCommand({create: collName, writeConcern: {w: 3}}));

    // Stop replication on the tiebreaker node, but keep it connected to the primary.
    stopServerReplication(tiebreakerNode);

    // Partition out the future primary so it doesn't receive the oplog entry.
    syncSource.disconnect(rollbackNode);
    syncSource.disconnect(tiebreakerNode);

    // Issue a 'prepareTransaction' command just to the current primary.
    const session = primary.getDB(dbName).getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({"prepare": "entry"}));
    const result = assert.commandWorked(
        session.getDatabase('admin').adminCommand({prepareTransaction: 1, writeConcern: {w: 1}}));
    assert(result.prepareTimestamp,
           "prepareTransaction did not return a 'prepareTimestamp': " + tojson(result));

    // TODO SERVER-38161: Remove this line once we have the functionality to step down without
    // having to commit or abort our prepared transactions. This 'commitTransaction' is currently
    // required to be able to proceed with the test. However, this makes the node crash on the
    // commit entry before getting to the prepare entry, and the goal of this test is to verify
    // it crashes on the prepare entry instead.
    PrepareHelpers.commitTransactionAfterPrepareTS(session, result.prepareTimestamp);

    // Shift the partition to only isolate the rollback node (current primary) instead.
    rollbackNode.disconnect(tiebreakerNode);

    // Issue an explicit stepdown command to save us some of the wait.
    try {
        rollbackNode.adminCommand({replSetStepDown: 60, force: true});
    } catch (e) {
        // Stepdown may fail if the node has already started stepping down. We might also
        // get an exception when the node closes the connection. Both are acceptable.
        print('Caught exception from replSetStepDown: ' + e);
    }

    // Wait for the old primary to finish stepping down and become a secondary.
    rst.waitForState(rollbackNode, ReplSetTest.State.SECONDARY);

    // Let the sync source step up in its place by giving it the third node's vote.
    syncSource.reconnect(tiebreakerNode);
    primary = rst.getPrimary();
    assert.eq(syncSource, primary);

    // Make sure the sync source has something new on it. We let the tiebreaker node replicate
    // that for some added test robustness.
    restartServerReplication(tiebreakerNode);
    assert.writeOK(
        syncSource.getDB(dbName)["syncSourceOnly"].insert({e: 1, writeConcern: {w: "majority"}}));

    clearRawMongoProgramOutput();

    // Reconnect the old primary to the rest of the set. We expect it to go into rollback.
    rollbackNode.reconnect(syncSource);
    rollbackNode.reconnect(tiebreakerNode);

    // Verify that it crashed because it failed to roll back.
    assert.soon(function() {
        try {
            rollbackNode.getDB("local").runCommand({ping: 1});
        } catch (e) {
            return true;
        }
        return false;
    }, "Node did not fassert", ReplSetTest.kDefaultTimeoutMS);

    rst.stop(rst.getNodeId(rollbackNode), undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

    var msg = RegExp("Can't roll back this command yet: ");
    assert.soon(function() {
        return rawMongoProgramOutput().match(msg);
    }, "Node did not fail to roll back entry.");

    rst.stopSet();
}());
