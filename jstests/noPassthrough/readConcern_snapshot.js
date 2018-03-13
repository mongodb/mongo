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
    let session =
        rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    let sessionDb = session.getDatabase(dbName);
    if (!sessionDb.serverStatus().storageEngine.supportsSnapshotReadConcern) {
        assert.commandFailedWithCode(
            sessionDb.runCommand(
                {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(0)}),
            ErrorCodes.IllegalOperation);
        rst.stopSet();
        return;
    }
    rst.stopSet();

    // readConcern 'snapshot' is not allowed on a standalone.
    const conn = MongoRunner.runMongod();
    session = conn.startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    assert.neq(null, conn, "mongod was unable to start up");
    assert.commandFailedWithCode(
        sessionDb.runCommand(
            {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(0)}),
        ErrorCodes.IllegalOperation);
    MongoRunner.stopMongod(conn);

    // readConcern 'snapshot' is allowed on a replica set primary.
    rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    session = rst.getPrimary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    let txnNumber = 0;
    assert.commandWorked(sessionDb.coll.insert({}, {w: 2}));
    assert.commandWorked(sessionDb.runCommand(
        {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)}));

    // readConcern 'snapshot' is allowed with 'afterClusterTime'.
    let pingRes = assert.commandWorked(rst.getPrimary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));
    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }));

    // readConcern 'snapshot' is not allowed with 'afterOpTime'.
    assert.commandFailedWithCode(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterOpTime: {ts: Timestamp(1, 2), t: 1}},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is allowed on a replica set secondary.
    session = rst.getSecondary().getDB(dbName).getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    assert.commandWorked(sessionDb.runCommand(
        {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)}));

    pingRes = assert.commandWorked(rst.getSecondary().adminCommand({ping: 1}));
    assert(pingRes.hasOwnProperty("$clusterTime"), tojson(pingRes));
    assert(pingRes.$clusterTime.hasOwnProperty("clusterTime"), tojson(pingRes));

    assert.commandWorked(sessionDb.runCommand({
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: pingRes.$clusterTime.clusterTime},
        txnNumber: NumberLong(txnNumber++)
    }));

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

    session = testDB.getMongo().startSession({causalConsistency: false});
    sessionDb = session.getDatabase(dbName);
    txnNumber = 0;

    // readConcern 'snapshot' is supported by find.
    assert.commandWorked(sessionDb.runCommand(
        {find: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)}));

    // readConcern 'snapshot' is supported by aggregate.
    assert.commandWorked(sessionDb.runCommand({
        aggregate: collName,
        pipeline: [],
        cursor: {},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));

    // readConcern 'snapshot' is supported by count.
    assert.commandWorked(sessionDb.runCommand(
        {count: collName, readConcern: {level: "snapshot"}, txnNumber: NumberLong(txnNumber++)}));

    // readConcern 'snapshot' is not supported by distinct.
    // TODO SERVER-33354: Add snapshot support for distinct.
    assert.commandFailedWithCode(sessionDb.runCommand({
        distinct: collName,
        key: "x",
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is not supported by geoNear.
    // TODO SERVER-33354: Add snapshot support for geoNear.
    assert.commandFailedWithCode(sessionDb.runCommand({
        geoNear: collName,
        near: [0, 0],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // readConcern 'snapshot' is supported by geoSearch.
    assert.commandWorked(sessionDb.runCommand({
        geoSearch: collName,
        near: [0, 0],
        maxDistance: 1,
        search: {a: 1},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));

    // readConcern 'snapshot' is supported by group.
    // TODO SERVER-33354: Add snapshot support for group.
    assert.commandFailedWithCode(sessionDb.runCommand({
        group: {ns: collName, key: {_id: 1}, $reduce: function(curr, result) {}, initial: {}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    // TODO SERVER-33592 Move all write related commands out of this test file when writes
    // with snapshot read concern are only allowed in transactions.
    // readConcern 'snapshot' is supported by insert.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "single-insert"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq({_id: "single-insert"}, sessionDb.coll.findOne({_id: "single-insert"}));

    // Wait for the last write to be committed since they will update the same session entry.
    // TODO SERVER-33592 remove the following write when writes with snapshot read concern
    // are only allowed in transactions.
    assert.commandWorked(
        sessionDb.coll.insert({_id: "dummy-insert"}, {writeConcern: {w: "majority"}}));
    // readConcern 'snapshot' is supported by batch insert.
    assert.commandWorked(sessionDb.runCommand({
        insert: collName,
        documents: [{_id: "batch-insert-1"}, {_id: "batch-insert-2"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq({_id: "batch-insert-1"}, sessionDb.coll.findOne({_id: "batch-insert-1"}));
    assert.eq({_id: "batch-insert-2"}, sessionDb.coll.findOne({_id: "batch-insert-2"}));

    // readConcern 'snapshot' is supported by update.
    assert.commandWorked(sessionDb.coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));
    printjson(assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: 0}, u: {$inc: {a: 1}}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    })));
    assert.eq({_id: 0, a: 1}, sessionDb.coll.findOne({_id: 0}));

    // readConcern 'snapshot' is supported by batch updates.
    assert.commandWorked(sessionDb.coll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDb.runCommand({
        update: collName,
        updates: [{q: {_id: 0}, u: {$inc: {a: 1}}}, {q: {_id: 1}, u: {$inc: {a: 1}}}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));
    assert.eq({_id: 0, a: 2}, sessionDb.coll.findOne({_id: 0}));
    assert.eq({_id: 1, a: 1}, sessionDb.coll.findOne({_id: 1}));

    // readConcern 'snapshot' is supported by delete.
    assert.commandWorked(sessionDb.coll.insert({_id: 2}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(sessionDb.runCommand({
        delete: collName,
        deletes: [{q: {}, limit: 1}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));

    // TODO SERVER-33591: Remove once snapshot writes use majority writeConcern.
    // Perform majority write to ensure any previous writes are part of the majority commit
    // snapshot.
    assert.commandWorked(sessionDb.coll.insert({}, {writeConcern: {w: "majority"}}));

    // readConcern 'snapshot' is supported by findAndModify.
    assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: 1},
        update: {$set: {b: 1}},
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
    }));
    assert.eq({_id: 1, a: 1, b: 1}, sessionDb.coll.findOne({_id: 1}));

    // TODO SERVER-33591: Remove once snapshot writes use majority writeConcern.
    // Perform majority write to ensure any previous writes are part of the majority commit
    // snapshot.
    assert.commandWorked(sessionDb.coll.insert({}, {writeConcern: {w: "majority"}}));

    assert.commandWorked(sessionDb.runCommand({
        findAndModify: collName,
        query: {_id: 1},
        remove: true,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++),
    }));
    assert.eq(0, sessionDb.coll.find({_id: 1}).itcount());

    // readConcern 'snapshot' is supported by parallelCollectionScan.
    assert.commandWorked(sessionDb.runCommand({
        parallelCollectionScan: collName,
        numCursors: 1,
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }));

    // readConcern 'snapshot' is not supported by non-CRUD commands.
    assert.commandFailedWithCode(sessionDb.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: "a_1"}],
        readConcern: {level: "snapshot"},
        txnNumber: NumberLong(txnNumber++)
    }),
                                 ErrorCodes.InvalidOptions);

    rst.stopSet();
}());
