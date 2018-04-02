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
    let txnNumber = 0;

    // 'atClusterTime' can be used with readConcern level 'snapshot'.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }));

    // 'atClusterTime' cannot be greater than the current cluster time.
    const futureClusterTime = new Timestamp(clusterTime.getTime() + 1000, 1);
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: futureClusterTime},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // 'atClusterTime' must have type Timestamp.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: "bad"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.TypeMismatch);

    // 'atClusterTime' cannot be used with readConcern level 'majority'.
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "majority", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with readConcern level 'local'.
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "local", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with readConcern level 'available'.
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "available", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with readConcern level 'linearizable'.
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "linearizable", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used without readConcern level (level is 'local' by default).
    assert.commandFailedWithCode(
        sessionDb.runCommand({find: collName, readConcern: {atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with 'afterOpTime'.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {
            level: "snapshot",
            atClusterTime: clusterTime,
            afterOpTime: {ts: Timestamp(1, 2), t: 1}
        },
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used outside of a session.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {find: collName, readConcern: {level: "snapshot", atClusterTime: clusterTime}}),
        ErrorCodes.InvalidOptions);

    // 'atClusterTime' cannot be used with 'afterClusterTime'.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: clusterTime, afterClusterTime: clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    session.endSession();
    rst.stopSet();

    // readConcern with 'atClusterTime' should fail when 'enableTestCommands' is set to false.
    {
        jsTest.setOption('enableTestCommands', false);
        let rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        let session =
            rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
        let sessionDb = session.getDatabase(dbName);
        assert.commandFailedWithCode(sessionDb.runCommand({
            find: collName,
            readConcern: {level: "snapshot", atClusterTime: _getClusterTime(rst)},
            txnNumber: NumberLong(0)
        }),
                                     ErrorCodes.InvalidOptions);
        session.endSession();
        rst.stopSet();

        jsTest.setOption('enableTestCommands', true);
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        session =
            rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
        sessionDb = session.getDatabase(dbName);
        assert.commandWorked(sessionDb.runCommand({
            find: collName,
            readConcern: {level: "snapshot", atClusterTime: _getClusterTime(rst)},
            txnNumber: NumberLong(0)
        }));
        session.endSession();
        rst.stopSet();
    }

}());
