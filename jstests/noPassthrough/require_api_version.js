/**
 * Tests the "requireApiVersion" mongod/mongos parameter.
 *
 * This test is incompatible with parallel and passthrough suites; concurrent jobs fail while
 * requireApiVersion is true.
 *
 * @tags: [
 *   requires_replication,
 *   requires_transactions,
 * ]
 */

(function() {
"use strict";

function runTest(db, supportsTransctions, writeConcern = {}, secondaries = []) {
    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: true}));
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, requireApiVersion: true}));
    }

    assert.commandFailedWithCode(db.runCommand({ping: 1}), 498870, "command without apiVersion");
    assert.commandWorked(db.runCommand({ping: 1, apiVersion: "1"}));
    assert.commandFailed(db.runCommand({ping: 1, apiVersion: "not a real API version"}));

    // Create a collection and do some writes with writeConcern majority.
    const collName = "testColl";
    assert.commandWorked(db.runCommand({create: collName, apiVersion: "1", writeConcern}));
    assert.commandWorked(db.runCommand(
        {insert: collName, documents: [{a: 1, b: 2}], apiVersion: "1", writeConcern}));

    // User management commands loop back into the system so make sure they set apiVersion
    // internally
    assert.commandWorked(db.adminCommand(
        {createRole: 'testRole', apiVersion: "1", writeConcern, privileges: [], roles: []}));
    assert.commandWorked(db.adminCommand({dropRole: 'testRole', apiVersion: "1", writeConcern}));

    /*
     * "getMore" accepts apiVersion.
     */
    assert.commandWorked(db.runCommand(
        {insert: "collection", documents: [{}, {}, {}], apiVersion: "1", writeConcern}));
    let reply = db.runCommand({find: "collection", batchSize: 1, apiVersion: "1"});
    assert.commandWorked(reply);
    assert.commandFailedWithCode(
        db.runCommand({getMore: reply.cursor.id, collection: "collection"}), 498870);
    assert.commandWorked(
        db.runCommand({getMore: reply.cursor.id, collection: "collection", apiVersion: "1"}));

    if (supportsTransctions) {
        /*
         * Commands in transactions require API version.
         */
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDb = session.getDatabase(db.getName());
        assert.commandFailedWithCode(sessionDb.runCommand({
            find: "collection",
            batchSize: 1,
            txnNumber: NumberLong(0),
            stmtId: NumberInt(2),
            startTransaction: true,
            autocommit: false
        }),
                                     498870);
        reply = sessionDb.runCommand({
            find: "collection",
            batchSize: 1,
            apiVersion: "1",
            txnNumber: NumberLong(1),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false
        });
        assert.commandWorked(reply);
        assert.commandFailedWithCode(sessionDb.runCommand({
            getMore: reply.cursor.id,
            collection: "collection",
            txnNumber: NumberLong(1),
            stmtId: NumberInt(1),
            autocommit: false
        }),
                                     498870);
        assert.commandWorked(sessionDb.runCommand({
            getMore: reply.cursor.id,
            collection: "collection",
            txnNumber: NumberLong(1),
            stmtId: NumberInt(1),
            autocommit: false,
            apiVersion: "1"
        }));

        assert.commandFailedWithCode(
            sessionDb.runCommand(
                {commitTransaction: 1, txnNumber: NumberLong(1), autocommit: false}),
            498870);

        assert.commandWorked(sessionDb.runCommand(
            {commitTransaction: 1, apiVersion: "1", txnNumber: NumberLong(1), autocommit: false}));

        // Start a new txn so we can test abortTransaction.
        reply = sessionDb.runCommand({
            find: "collection",
            apiVersion: "1",
            txnNumber: NumberLong(2),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false
        });
        assert.commandWorked(reply);
        assert.commandFailedWithCode(
            sessionDb.runCommand(
                {abortTransaction: 1, txnNumber: NumberLong(2), autocommit: false}),
            498870);
        assert.commandWorked(sessionDb.runCommand(
            {abortTransaction: 1, apiVersion: "1", txnNumber: NumberLong(2), autocommit: false}));
    }

    assert.commandWorked(
        db.runCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    for (const secondary of secondaries) {
        assert.commandWorked(
            secondary.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    }
    assert.commandWorked(db.runCommand({ping: 1}));
}

function requireApiVersionOnShardOrConfigServerTest() {
    assert.throws(
        () => MongoRunner.runMongod(
            {shardsvr: "", replSet: "dummy", setParameter: {"requireApiVersion": true}}),
        [],
        "mongod should not be able to start up with --shardsvr and requireApiVersion=true");

    assert.throws(
        () => MongoRunner.runMongod(
            {configsvr: "", replSet: "dummy", setParameter: {"requireApiVersion": 1}}),
        [],
        "mongod should not be able to start up with --configsvr and requireApiVersion=true");

    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();
    const singleNodeShard = rs.getPrimary();
    assert.neq(null, singleNodeShard, "mongod was not able to start up");
    assert.commandFailed(
        singleNodeShard.adminCommand({setParameter: 1, requireApiVersion: true}),
        "should not be able to set requireApiVersion=true on mongod that was started with --shardsvr");
    rs.stopSet();

    const configsvrRS = new ReplSetTest({nodes: 1});
    configsvrRS.startSet({configsvr: ""});
    configsvrRS.initiate();
    const configsvrConn = configsvrRS.getPrimary();
    assert.neq(null, configsvrConn, "mongod was not able to start up");
    assert.commandFailed(
        configsvrConn.adminCommand({setParameter: 1, requireApiVersion: 1}),
        "should not be able to set requireApiVersion=true on mongod that was started with --configsvr");
    configsvrRS.stopSet();
}

requireApiVersionOnShardOrConfigServerTest();

const mongod = MongoRunner.runMongod();
runTest(mongod.getDB("admin"), false /* supportsTransactions */);
MongoRunner.stopMongod(mongod);

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiateWithHighElectionTimeout();

runTest(rst.getPrimary().getDB("admin"),
        true /* supportsTransactions */,
        {w: "majority"} /* writeConcern */,
        rst.getSecondaries());
rst.stopSet();

const st = new ShardingTest({});
runTest(st.s0.getDB("admin"), true /* supportsTransactions */);
st.stop();
}());
