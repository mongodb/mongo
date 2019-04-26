// Test parsing of readConcern level 'snapshot' on mongos.
// @tags: [requires_replication,requires_sharding, uses_transactions, uses_atclustertime]
(function() {
    "use strict";

    load("jstests/sharding/libs/sharded_transactions_helpers.js");

    // Runs the command as the first in a multi statement txn that is aborted right after, expecting
    // success.
    function expectSuccessInTxnThenAbort(session, sessionConn, cmdObj) {
        session.startTransaction();
        assert.commandWorked(sessionConn.runCommand(cmdObj));
        session.abortTransaction_forTesting();
    }

    // Runs the command as the first in a multi statement txn that is aborted right after, expecting
    // failure with the given error code.
    function expectFailInTxnThenAbort(session, sessionConn, expectedErrorCode, cmdObj) {
        session.startTransaction();
        assert.commandFailedWithCode(sessionConn.runCommand(cmdObj), expectedErrorCode);
        session.abortTransaction_forTesting();
    }

    const dbName = "test";
    const collName = "coll";

    let st = new ShardingTest({shards: 1, rs: {nodes: 2}, config: 2, mongos: 1});
    let testDB = st.getDB(dbName);
    let coll = testDB.coll;

    // Insert data to create the collection.
    assert.writeOK(testDB[collName].insert({x: 1}));

    flushRoutersAndRefreshShardMetadata(st, {ns: dbName + "." + collName, dbNames: [dbName]});

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

    expectFailInTxnThenAbort(session, sessionDb, ErrorCodes.InvalidOptions, {
        find: collName,
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
    });

    // Passthrough tests. There are parts not implemented on mongod and mongos, they are tracked by
    // separate jiras

    // readConcern 'snapshot' is supported by insert on mongos in a transaction.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        insert: collName,
        documents: [{_id: "single-insert"}],
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is supported by update on mongos in a transaction.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        update: collName,
        updates: [{q: {_id: 0}, u: {$inc: {a: 1}}}],
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is supported by delete on mongos in a transaction.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        delete: collName,
        deletes: [{q: {}, limit: 1}],
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is supported by findAndModify on mongos in a transaction.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        findAndModify: collName,
        filter: {},
        update: {$set: {a: 1}},
        readConcern: {level: "snapshot"},
    });

    expectSuccessInTxnThenAbort(session, sessionDb, {
        aggregate: collName,
        pipeline: [],
        cursor: {},
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is supported by find on mongos.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        find: collName,
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is supported by distinct on mongos.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        distinct: collName,
        key: "x",
        readConcern: {level: "snapshot"},
    });

    // readConcern 'snapshot' is allowed with 'afterClusterTime'.
    expectSuccessInTxnThenAbort(session, sessionDb, {
        find: collName,
        readConcern: {level: "snapshot", afterClusterTime: clusterTime},
    });

    expectSuccessInTxnThenAbort(session, sessionDb, {
        aggregate: collName,
        pipeline: [],
        cursor: {},
        readConcern: {level: "snapshot", afterClusterTime: clusterTime},
    });

    st.stop();
}());
