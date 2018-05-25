// Test parsing of readConcern level 'snapshot'.
// @tags: [requires_replication,requires_sharding]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    let st = new ShardingTest({shards: 1, rs: {nodes: 2}, config: 2, mongos: 1});
    let testDB = st.getDB(dbName);
    let coll = testDB.coll;

    let shardDB = st.rs0.getPrimary().getDB(dbName);
    if (!shardDB.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        st.stop();
        return;
    }

    // noPassthrough tests
    // readConcern 'snapshot' is not allowed outside session context.
    assert.commandFailedWithCode(
        testDB.runCommand({find: collName, readConcern: {level: "snapshot"}}),
        ErrorCodes.InvalidOptions);

    let session = testDB.getMongo().startSession({causalConsistency: false});
    let sessionDb = session.getDatabase(dbName);

    // readConcern 'snapshot' is not allowed outside transaction context.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot"},
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not allowed with 'atClusterTime'.
    let pingRes = assert.commandWorked(st.s0.adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    const clusterTime = pingRes.$clusterTime.clusterTime;
    let txnNumber = 0;

    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by insert on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "single-insert"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by update on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: 0}, u: {$inc: {a: 1}}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by delete on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        delete: collName,
        deletes: [{q: {}, limit: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by findAndModify on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        findAndModify: collName,
        filter: {},
        update: {$set: {a: 1}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by non-CRUD commands.
    assert.commandFailedWithCode(sessionDb.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "a_1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // Passthrough tests. There are parts not implemented on mongod and mongos, they are tracked by
    // separate jiras
    assert.commandWorked(sessionDb.runCommand({
        aggregate: collName,
        pipeline: [],
        cursor: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));

    // readConcern 'snapshot' is supported by find on mongos.
    assert.commandWorked(sessionDb.runCommand(
        {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)}));

    // readConcern 'snapshot' is allowed with 'afterClusterTime'.
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.commandWorked(sessionDb.runCommand({
        aggregate: collName,
        pipeline: [],
        cursor: {},
        readConcern: {level: "snapshot", afterClusterTime: clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }));

    assert.commandWorked(sessionDb.coll.insert({}, {w: 2}));
    assert.commandWorked(coll.createIndex({geo: "2d"}));
    assert.commandWorked(coll.createIndex({haystack: "geoHaystack", a: 1}, {bucketSize: 1}));

    // Passthrough tests that are not implemented yet.

    // TODO SERVER-33709: Add snapshot support for cluster count on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        count: collName,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // TODO SERVER-33710: Add snapshot support for distinct on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        distinct: collName,
        key: "x",
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // TODO SERVER-33712: Add snapshot support for geoNear on mongos.
    assert.commandFailedWithCode(sessionDb.runCommand({
        geoNear: collName,
        near: [0, 0],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    st.stop();
}());
