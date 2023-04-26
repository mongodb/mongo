"use strict";

/**
 * Library used to test that prepared transactions survive failovers due to reconfig.
 */

var testPrepareFailoverDueToReconfig = function(name, reconfigOnPrimary) {
    load("jstests/core/txns/libs/prepare_helpers.js");

    const dbName = "test";
    const collName = name;

    const rst = new ReplSetTest({name: name, nodes: 2});
    const nodes = rst.nodeList();

    rst.startSet();
    rst.initiate({
        "_id": name,
        "members": [
            {/* primary   */ "_id": 0, "host": nodes[0]},
            {/* secondary */ "_id": 1, "host": nodes[1], "priority": 0}
        ]
    });

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    const testDB = primary.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    const oldDoc = {_id: 42, "is": "old"};
    const newDoc = {_id: 42, "is": "new"};

    // First create the collection.
    assert.commandWorked(testColl.insert(oldDoc));

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.update(oldDoc, newDoc));

    // Prepare a transaction. This will be replicated to the secondary.
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    // Now reconfig to force a failover.
    let config = rst.getReplSetConfigFromNode();

    config.members[0].priority = 0;
    config.members[1].priority = 1;
    config.version++;

    // Run the reconfig command on whichever node the caller targeted.
    const reconfigTarget = reconfigOnPrimary ? primary : secondary;
    assert.commandWorked(reconfigTarget.adminCommand({replSetReconfig: config, force: true}));
    rst.waitForState(primary, ReplSetTest.State.SECONDARY);

    // Wait for the old secondary to become the new primary.
    const newPrimary = rst.getPrimary();
    assert.neq(primary, newPrimary, "failover did not occur");

    // Commit the prepared transaction on the new primary.
    const newSession = new _DelegatingDriverSession(newPrimary, session);
    assert.commandWorked(PrepareHelpers.commitTransaction(newSession, prepareTimestamp));

    // Verify the effect of the transaction.
    const doc = newPrimary.getDB(dbName).getCollection(collName).findOne({});
    assert.docEq(newDoc, doc);

    rst.stopSet();
};
