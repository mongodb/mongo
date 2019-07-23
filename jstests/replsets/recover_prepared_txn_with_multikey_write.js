/**
 * Test that replication recovery can reconstruct a prepared transaction that includes a write that
 * sets the multikey flag.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
              // Disallow elections on secondary.
              rsConfig: {
                  priority: 0,
                  votes: 0,
              }
            }
        ]
    });

    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const session = primary.getDB("test").getMongo().startSession();
    const sessionDB = session.getDatabase("test");
    const sessionColl = sessionDB.getCollection("coll");

    // Create an index that will later be made multikey.
    sessionColl.createIndex({x: 1});
    session.startTransaction();

    // Make the index multikey.
    sessionColl.insert({x: [1, 2, 3]});
    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));

    // Do an unclean shutdown so we don't force a checkpoint, and then restart.
    rst.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
    rst.restart(0);

    rst.stopSet();
}());
