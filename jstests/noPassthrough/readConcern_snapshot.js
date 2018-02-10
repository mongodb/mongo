// Test parsing of readConcern level 'snapshot'.
// @tags: [requires_replication]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    //
    // Configurations.
    //

    // readConcern 'snapshot' should fail on storage engines that do not support it.
    let rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    if (!rst.getPrimary().getDB(dbName).serverStatus().storageEngine.supportsSnapshotReadConcern) {
        assert.commandFailedWithCode(rst.getPrimary().getDB(dbName).runCommand(
                                         {find: collName, readConcern: {level: "snapshot"}}),
                                     ErrorCodes.InvalidOptions);
        rst.stopSet();
        return;
    }
    rst.stopSet();

    // readConcern 'snapshot' is not allowed on a standalone.
    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");
    assert.commandFailedWithCode(
        conn.getDB(dbName).runCommand({find: collName, readConcern: {level: "snapshot"}}),
        ErrorCodes.NotAReplicaSet);
    MongoRunner.stopMongod(conn);

    // readConcern 'snapshot' is not allowed on mongos.
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    assert.commandFailedWithCode(
        st.getDB(dbName).runCommand({find: collName, readConcern: {level: "snapshot"}}),
        ErrorCodes.InvalidOptions);
    st.stop();

    // readConcern 'snapshot' is not allowed with protocol version 0.
    rst = new ReplSetTest({nodes: 1, protocolVersion: 0});
    rst.startSet();
    rst.initiate();
    assert.commandFailedWithCode(rst.getPrimary().getDB(dbName).runCommand(
                                     {find: collName, readConcern: {level: "snapshot"}}),
                                 ErrorCodes.IncompatibleElectionProtocol);
    rst.stopSet();

    // readConcern 'snapshot' is allowed on a replica set primary.
    rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    assert.commandWorked(rst.getPrimary().getDB(dbName).coll.insert({}, {w: 2}));
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand(
        {find: collName, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is allowed on a replica set secondary.
    assert.commandWorked(rst.getSecondary().getDB(dbName).runCommand(
        {find: collName, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is allowed with 'afterClusterTime'.
    const pingRes = assert.commandWorked(rst.getPrimary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime}
    }));

    // readConcern 'snapshot' is not allowed with 'afterOpTime'.
    assert.commandFailedWithCode(rst.getPrimary().getDB(dbName).runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterOpTime: {ts: Timestamp(1, 2), t: 1}}
    }),
                                 ErrorCodes.InvalidOptions);
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
    assert.commandWorked(coll.createIndex({haystack: "geoHaystack", a: 1}, {bucketSize: 1}));

    // readConcern 'snapshot' is supported by aggregate.
    assert.commandWorked(testDB.runCommand(
        {aggregate: collName, pipeline: [], cursor: {}, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by count.
    assert.commandWorked(testDB.runCommand({count: collName, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by distinct.
    assert.commandWorked(testDB.runCommand({count: collName, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by find.
    assert.commandWorked(testDB.runCommand({find: collName, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by geoNear.
    assert.commandWorked(
        testDB.runCommand({geoNear: collName, near: [0, 0], readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by geoSearch.
    assert.commandWorked(testDB.runCommand({
        geoSearch: collName,
        near: [0, 0],
        maxDistance: 1,
        search: {a: 1},
        readConcern: {level: "snapshot"}
    }));

    // readConcern 'snapshot' is supported by group.
    assert.commandWorked(testDB.runCommand({
        group: {ns: collName, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}},
        readConcern: {level: "snapshot"}
    }));

    // readConcern 'snapshot' is supported by insert.
    assert.commandWorked(
        testDB.runCommand({insert: collName, documents: [{}], readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by update.
    assert.commandWorked(testDB.runCommand({
        update: collName,
        updates: [{q: {}, u: {$set: {a: 1}}}],
        readConcern: {level: "snapshot"}
    }));

    // readConcern 'snapshot' is supported by delete.
    assert.commandWorked(testDB.runCommand(
        {delete: collName, deletes: [{q: {}, limit: 1}], readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is supported by findAndModify.
    assert.commandWorked(testDB.runCommand({
        findAndModify: collName,
        filter: {},
        update: {$set: {a: 1}},
        readConcern: {level: "snapshot"}
    }));

    // readConcern 'snapshot' is supported by parallelCollectionScan.
    assert.commandWorked(testDB.runCommand(
        {parallelCollectionScan: collName, numCursors: 1, readConcern: {level: "snapshot"}}));

    // readConcern 'snapshot' is not supported by non-CRUD commands.
    assert.commandFailedWithCode(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "a_1"}],
        readConcern: {level: "snapshot"}
    }),
                                 ErrorCodes.InvalidOptions);

    rst.stopSet();
}());
