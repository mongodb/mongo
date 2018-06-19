// Test parsing of readConcern level 'snapshot'.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    //
    // Configurations.
    //

    // Transactions should fail on storage engines that do not support them.
    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    let session =
        rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    let sessionDb = session.getDatabase(dbName);
    if (!sessionDb.serverStatus().storageEngine.supportsSnapshotReadConcern ||
        !sessionDb.serverStatus().storageEngine.persistent) {
        // Transactions with readConcern snapshot fail.
        session.startTransaction({readConcern: {level: "snapshot"}});
        assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                     ErrorCodes.IllegalOperation);
        session.abortTransaction();

        // Transactions without readConcern snapshot fail.
        session.startTransaction();
        assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                     ErrorCodes.IllegalOperation);
        session.abortTransaction();

        rst.stopSet();
        return;
    }
    session.endSession();
    rst.stopSet();

    // TODO: SERVER-34113:
    // readConcern 'snapshot' should fail for autocommit:true transactions when test
    // 'enableTestCommands' is set to false.
    jsTest.setOption('enableTestCommands', false);
    rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    assert.commandWorked(sessionDb.coll.insert({}, {writeConcern: {w: "majority"}}));
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(1)}),
        ErrorCodes.InvalidOptions);
    jsTest.setOption('enableTestCommands', true);
    session.endSession();
    rst.stopSet();

    // readConcern 'snapshot' is not allowed on a standalone.
    const conn = MongoRunner.runMongod();
    session = conn.startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    assert.neq(null, conn, "mongod was unable to start up");
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}),
                                 ErrorCodes.IllegalOperation);
    session.abortTransaction();
    session.endSession();
    MongoRunner.stopMongod(conn);

    // readConcern 'snapshot' is allowed on a replica set primary.
    rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand(
        {create: collName, writeConcern: {w: "majority"}}));
    session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    session.startTransaction({writeConcern: {w: "majority"}, readConcern: {level: "snapshot"}});
    assert.commandWorked(sessionDb.coll.insert({}));
    assert.commandWorked(sessionDb.runCommand({find: collName}));
    session.commitTransaction();

    // readConcern 'snapshot' is allowed with 'afterClusterTime'.
    session.startTransaction();
    let pingRes = assert.commandWorked(rst.getPrimary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime}
    }));
    session.commitTransaction();

    // readConcern 'snapshot' is not allowed with 'afterOpTime'.
    session.startTransaction(
        {readConcern: {level: "snapshot", afterOpTime: {ts: Timestamp(1, 2), t: 1}}});
    assert.commandFailedWithCode(sessionDb.runCommand({find: collName}), ErrorCodes.InvalidOptions);
    session.abortTransaction();
    session.endSession();

    // readConcern 'snapshot' is allowed on a replica set secondary.
    rst.awaitLastOpCommitted();
    session = rst.getSecondary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(sessionDb.runCommand({find: collName}));
    session.commitTransaction();

    pingRes = assert.commandWorked(rst.getSecondary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));

    session.startTransaction(
        {readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime}});
    assert.commandWorked(sessionDb.runCommand({find: collName}));
    session.commitTransaction();

    session.endSession();
    rst.stopSet();

    //
    // Commands.
    //

    rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    let testDB = rst.getPrimary().getDB(dbName);
    let coll = testDB.coll;
    assert.commandWorked(coll.createIndex({geo: "2d"}));
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}],
        writeConcern: {w: "majority"}
    }));

    session = testDB.getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);

    // readConcern 'snapshot' is supported by find.
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
    assert.commandWorked(sessionDb.runCommand({find: collName}));

    // readConcern 'snapshot' is supported by aggregate.
    assert.commandWorked(sessionDb.runCommand({aggregate: collName, pipeline: [], cursor: {}}));

    // readConcern 'snapshot' is supported by distinct.
    assert.commandWorked(sessionDb.runCommand({distinct: collName, key: "x"}));

    // readConcern 'snapshot' is supported by geoSearch.
    assert.commandWorked(
        sessionDb.runCommand({geoSearch: collName, near: [0, 0], maxDistance: 1, search: {a: 1}}));

    // readConcern 'snapshot' is not supported by non-CRUD commands.
    assert.commandFailedWithCode(
        sessionDb.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}),
        50768);
    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();

    rst.stopSet();
}());
