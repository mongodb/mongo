// Test parsing of readConcern option 'atClusterTime'.
//
// Only run this test with the WiredTiger storage engine, since we expect other storage engines to
// return early because they do not support snapshot read concern.
// @tags: [requires_wiredtiger]

function _getClusterTime(rst) {
    const pingRes = assert.commandWorked(rst.getPrimary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    return pingRes.$clusterTime.clusterTime;
}

(function() {
    "use strict";

    // Skip this test if running with --nojournal and WiredTiger.
    if (jsTest.options().noJournal &&
        (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
        print("Skipping test because running WiredTiger without journaling isn't a valid" +
              " replica set configuration");
        return;
    }

    const dbName = "test";
    const collName = "coll";

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const testDB = rst.getPrimary().getDB(dbName);

    if (!testDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        rst.stopSet();
        return;
    }

    const session = testDB.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);

    const clusterTime = _getClusterTime(rst);

    // 'atClusterTime' can be used with readConcern level 'snapshot'.
    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: clusterTime}});
    assert.commandWorked(sessionDb.runCommand({find: collName}));
    session.commitTransaction();

    // 'atClusterTime' cannot be greater than the current cluster time.
    const futureClusterTime = new Timestamp(clusterTime.getTime() + 1000, 1);
    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: futureClusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' must have type Timestamp.
    session.startTransaction({readConcern: {level: "snapshot", atClusterTime: "bad"}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.TypeMismatch);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used with readConcern level 'majority'.
    session.startTransaction({readConcern: {level: "majority", atClusterTime: clusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used with readConcern level 'local'.
    session.startTransaction({readConcern: {level: "local", atClusterTime: clusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used with readConcern level 'available'.
    session.startTransaction({readConcern: {level: "available", atClusterTime: clusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used with readConcern level 'linearizable'.
    session.startTransaction({readConcern: {level: "linearizable", atClusterTime: clusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used without readConcern level (level is 'local' by default).
    session.startTransaction({readConcern: {atClusterTime: clusterTime}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used with 'afterOpTime'.
    session.startTransaction({
        readConcern: {
            level: "snapshot",
            atClusterTime: clusterTime,
            afterOpTime: {ts: Timestamp(1, 2), t: 1}
        }
    });
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // 'atClusterTime' cannot be used outside of a session.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {find: collName, readConcern: {level: "snapshot", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with 'afterClusterTime'.
    session.startTransaction({
        readConcern:
            {level: "snapshot", atClusterTime: clusterTime, afterClusterTime: clusterTime}
    });
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();
    rst.stopSet();

    // readConcern with 'atClusterTime' should fail when 'enableTestCommands' is set to false.
    // TODO: SERVER-35643 Allow atClusterTime when enableTestCommands is false.
    {
        jsTest.setOption('enableTestCommands', false);
        let rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        let session =
            rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
        let sessionDb = session.getDatabase(dbName);
        session.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: _getClusterTime(rst)}});
        assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
        session.endSession();
        rst.stopSet();

        jsTest.setOption('enableTestCommands', true);
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        session =
            rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
        sessionDb = session.getDatabase(dbName);
        session.startTransaction(
            {readConcern: {level: "snapshot", atClusterTime: _getClusterTime(rst)}});
        assert.commandWorked(sessionDb.runCommand({find: collName}));
        session.commitTransaction();
        session.endSession();
        rst.stopSet();
    }

}());
