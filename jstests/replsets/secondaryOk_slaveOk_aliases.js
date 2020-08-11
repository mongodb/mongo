// Tests that member functions setSecondaryOk()/getSecondaryOk() and their
// aliases, setSlaveOk()/getSlaveOk(), produce the same results.

(function() {
    "use strict";
    const dbName = "test";
    const collName = "coll";
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    assert.writeOK(primary.getDB(dbName)[collName].insert({x: 1}));
    rst.awaitReplication();

    // secondaryOk is initially set to true in awaitReplication, so reads on secondaries should
    // succeed.
    assert.eq(secondary.getDB(dbName).getMongo().getSecondaryOk(), true);
    assert.eq(secondary.getDB(dbName).getSecondaryOk(), true);
    assert.commandWorked(secondary.getDB(dbName).runCommand({find: collName}),
                         "find command failed with an unexpected error");

    // Set secondaryOk to false, disallowing reads on secondaries.
    secondary.getDB(dbName).getMongo().setSecondaryOk(false);
    assert.eq(secondary.getDB(dbName).getMongo().getSecondaryOk(), false);
    assert.commandFailedWithCode(secondary.getDB(dbName).runCommand({find: collName}),
                                 ErrorCodes.NotMasterNoSlaveOk,
                                 "find did not fail with the correct error code");

    // setSlaveOk() is deprecated and aliased to setSecondaryOk(), but ensure
    // it still works for backwards compatibility.
    secondary.getDB(dbName).getMongo().setSlaveOk();
    assert.eq(secondary.getDB(dbName).getMongo().getSlaveOk(), true);
    assert.eq(secondary.getDB(dbName).getSlaveOk(), true);
    assert.commandWorked(secondary.getDB(dbName).runCommand({find: collName}),
                         "find command failed with an unexpected error");

    // Set slaveOk to false, disallowing reads on secondaries.
    secondary.getDB(dbName).getMongo().setSlaveOk(false);
    assert.eq(secondary.getDB(dbName).getMongo().getSlaveOk(), false);
    assert.commandFailedWithCode(secondary.getDB(dbName).runCommand({find: collName}),
                                 ErrorCodes.NotMasterNoSlaveOk,
                                 "find did not fail with the correct error code");
    rst.stopSet();
})();
