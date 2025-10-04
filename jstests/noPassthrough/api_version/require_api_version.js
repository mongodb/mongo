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

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(db, supportsTransactions, writeConcern = {}, secondaries = []) {
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
    assert.commandWorked(db.runCommand({insert: collName, documents: [{a: 1, b: 2}], apiVersion: "1", writeConcern}));

    // User management commands loop back into the system so make sure they set apiVersion
    // internally
    assert.commandWorked(
        db.adminCommand({createRole: "testRole", apiVersion: "1", writeConcern, privileges: [], roles: []}),
    );
    assert.commandWorked(db.adminCommand({dropRole: "testRole", apiVersion: "1", writeConcern}));

    /*
     * "getMore" accepts apiVersion.
     */
    assert.commandWorked(db.runCommand({insert: "collection", documents: [{}, {}, {}], apiVersion: "1", writeConcern}));
    let reply = db.runCommand({find: "collection", batchSize: 1, apiVersion: "1"});
    assert.commandWorked(reply);
    assert.commandFailedWithCode(db.runCommand({getMore: reply.cursor.id, collection: "collection"}), 498870);
    assert.commandWorked(db.runCommand({getMore: reply.cursor.id, collection: "collection", apiVersion: "1"}));

    if (supportsTransactions) {
        /*
         * Commands in transactions require API version.
         */
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDb = session.getDatabase(db.getName());
        assert.commandFailedWithCode(
            sessionDb.runCommand({
                find: "collection",
                batchSize: 1,
                txnNumber: NumberLong(0),
                stmtId: NumberInt(2),
                startTransaction: true,
                autocommit: false,
            }),
            498870,
        );
        reply = sessionDb.runCommand({
            find: "collection",
            batchSize: 1,
            apiVersion: "1",
            txnNumber: NumberLong(1),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        });
        assert.commandWorked(reply);
        assert.commandFailedWithCode(
            sessionDb.runCommand({
                getMore: reply.cursor.id,
                collection: "collection",
                txnNumber: NumberLong(1),
                stmtId: NumberInt(1),
                autocommit: false,
            }),
            498870,
        );
        assert.commandWorked(
            sessionDb.runCommand({
                getMore: reply.cursor.id,
                collection: "collection",
                txnNumber: NumberLong(1),
                stmtId: NumberInt(1),
                autocommit: false,
                apiVersion: "1",
            }),
        );

        assert.commandFailedWithCode(
            sessionDb.runCommand({commitTransaction: 1, txnNumber: NumberLong(1), autocommit: false}),
            498870,
        );

        assert.commandWorked(
            sessionDb.runCommand({commitTransaction: 1, apiVersion: "1", txnNumber: NumberLong(1), autocommit: false}),
        );

        // Start a new txn so we can test abortTransaction.
        reply = sessionDb.runCommand({
            find: "collection",
            apiVersion: "1",
            txnNumber: NumberLong(2),
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        });
        assert.commandWorked(reply);
        assert.commandFailedWithCode(
            sessionDb.runCommand({abortTransaction: 1, txnNumber: NumberLong(2), autocommit: false}),
            498870,
        );
        assert.commandWorked(
            sessionDb.runCommand({abortTransaction: 1, apiVersion: "1", txnNumber: NumberLong(2), autocommit: false}),
        );
    }

    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    }
    assert.commandWorked(db.runCommand({ping: 1}));
}

function requireApiVersionOnShardOrConfigServerTest() {
    assert.throws(
        () => MongoRunner.runMongod({shardsvr: "", replSet: "dummy", setParameter: {"requireApiVersion": true}}),
        [],
        "mongod should not be able to start up with --shardsvr and requireApiVersion=true",
    );

    assert.throws(
        () => MongoRunner.runMongod({configsvr: "", replSet: "dummy", setParameter: {"requireApiVersion": 1}}),
        [],
        "mongod should not be able to start up with --configsvr and requireApiVersion=true",
    );

    const rs = new ReplSetTest({nodes: 1});
    rs.startSet({shardsvr: ""});
    rs.initiate();
    const singleNodeShard = rs.getPrimary();
    assert.neq(null, singleNodeShard, "mongod was not able to start up");
    assert.commandFailed(
        singleNodeShard.adminCommand({setParameter: 1, requireApiVersion: true}),
        "should not be able to set requireApiVersion=true on mongod that was started with --shardsvr",
    );
    rs.stopSet();

    const configsvrRS = new ReplSetTest({nodes: 1});
    configsvrRS.startSet({configsvr: ""});
    configsvrRS.initiate();
    const configsvrConn = configsvrRS.getPrimary();
    assert.neq(null, configsvrConn, "mongod was not able to start up");
    assert.commandFailed(
        configsvrConn.adminCommand({setParameter: 1, requireApiVersion: 1}),
        "should not be able to set requireApiVersion=true on mongod that was started with --configsvr",
    );
    configsvrRS.stopSet();
}

function checkLogsForHelloFromReplCoordExternNetwork(logs) {
    for (let logMsg of logs) {
        let obj = JSON.parse(logMsg);
        if (
            checkLog.compareLogs(
                obj,
                21965, // Search for "About to run the command" logs.
                "D2",
                null,
                {
                    "commandArgs": {
                        "hello": 1,
                        "client": {"driver": {"name": "NetworkInterfaceTL-ReplCoordExternNetwork"}},
                    },
                },
                true,
            )
        ) {
            return true;
        }
    }
    return false;
}

// Test that internal "hello" commands bypass `requireApiVersion` checks. We drop connections before
// certain internal commands to force "hello" to be sent. As an example, use `insert`
// with "majority" write concern in order to trigger internal commands to be sent from the secondary
// to the primary.
function requireApiVersionDropConnectionTest() {
    const rst = new ReplSetTest({nodes: 3, nodeOptions: {setParameter: {logComponentVerbosity: tojson({command: 2})}}});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB("admin");
    const secondaries = rst.getSecondaries();

    // Drop "ReplCoordExternNetwork" connections on secondary to force new connections and "hello"s
    // to be sent.
    for (const secondary of secondaries) {
        assert.commandWorked(
            secondary.getDB("admin").runCommand({
                configureFailPoint: "connectionPoolDropConnectionsBeforeGetConnection",
                mode: "alwaysOn",
                data: {"instance": "NetworkInterfaceTL-ReplCoordExternNetwork"},
            }),
        );
    }

    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: true}));
    for (const secondary of secondaries) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, requireApiVersion: true}));
    }

    // During internal `replSetUpdatePosition` from secondaries to the primary, connections will be
    // dropped, and new "hello"s will be sent. These "hello"s should bypass any `requireApiVersion`
    // check.
    assert.commandWorked(
        db.runCommand({
            insert: "testColl",
            documents: [{a: 1, b: 2}],
            apiVersion: "1",
            writeConcern: {w: "majority"},
        }),
    );

    // Check that internal "hello"s triggered by `replSetUpdatePosition` were processed.
    let logs = assert.commandWorked(db.adminCommand({getLog: "global", apiVersion: "1"})).log;
    assert(checkLogsForHelloFromReplCoordExternNetwork(logs));

    assert.commandWorked(db.runCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    for (const secondary of rst.getSecondaries()) {
        assert.commandWorked(secondary.adminCommand({setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
    }

    assert.commandWorked(
        db.runCommand({configureFailPoint: "connectionPoolDropConnectionsBeforeGetConnection", mode: "off"}),
    );
    rst.stopSet();
}

requireApiVersionOnShardOrConfigServerTest();

requireApiVersionDropConnectionTest();

const mongod = MongoRunner.runMongod();
runTest(mongod.getDB("admin"), false /* supportsTransactions */);
MongoRunner.stopMongod(mongod);

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

runTest(
    rst.getPrimary().getDB("admin"),
    true /* supportsTransactions */,
    {w: "majority"} /* writeConcern */,
    rst.getSecondaries(),
);
rst.stopSet();

const st = new ShardingTest({});
runTest(st.s0.getDB("admin"), true /* supportsTransactions */);
st.stop();
