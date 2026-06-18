/**
 * Utilities for testing writeConcern.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Extension point for fixtures with a non-standard stall mechanism. If a handler is registered
// here, it's called first; return true to skip the default dispatch. Mirrors the override-slot
// pattern used by `kOverrideConstructor` in `replsettest.js`.
export const kReplicationStallOverride = Symbol("replicationStallOverrideHandler");
export const replicationStallExtensionPoint = {};

function _toggleReplicationStall(conn, enabled) {
    if (Array.isArray(conn) && conn.length === 0) {
        return;
    }

    // The override receives a single representative node (not the full array) and is expected to
    // act fixture-wide on its own; return true to skip the default per-conn dispatch entirely.
    const override = replicationStallExtensionPoint[kReplicationStallOverride];
    if (override) {
        const node = Array.isArray(conn) ? conn[0] : conn;
        if (override(node, enabled)) {
            return;
        }
    }

    if (Array.isArray(conn)) {
        conn.forEach((n) => _toggleReplicationStall(n, enabled));
        return;
    }

    if (!enabled) {
        configureFailPoint(conn, "stopReplProducer", {}, "off");
        return;
    }
    const stopReplProducerFailPoint = configureFailPoint(conn, "stopReplProducer");

    // Wait until the fail point is actually hit. Don't wait if the node is the primary, because
    // the fail point won't be hit until the node transitions from being the primary.
    if (
        assert.commandWorked(conn.adminCommand("replSetGetStatus")).myState !=
        ReplSetTest.State.PRIMARY
    ) {
        stopReplProducerFailPoint.wait();
    }
}

// Shards a collection with 'numDocs' documents and creates 2 chunks, one on each of two shards.
export function shardCollectionWithChunks(st, coll, numDocs) {
    let _db = coll.getDB();
    let numberDoc = numDocs || 20;
    coll.createIndex({x: 1}, {unique: true});

    st.shardColl(
        coll.getName(),
        {x: 1},
        {x: numberDoc / 2},
        {x: numberDoc / 2},
        _db.toString(),
        true,
    );

    for (let i = 0; i < numberDoc; i++) {
        coll.insert({x: i});
    }
    assert.eq(coll.count(), numberDoc);
}

// Stops replication on the given server(s).
export function stopServerReplication(conn) {
    _toggleReplicationStall(conn, true);
}

// Stops replication at all replicaset secondaries. However, it might wait for replication before
// stopping it.
export function stopReplicationOnSecondaries(rs, changeReplicaSetDefaultWCToLocal = true) {
    if (changeReplicaSetDefaultWCToLocal == true) {
        // The default WC is majority and this test can't satisfy majority writes.
        assert.commandWorked(
            rs.getPrimary().adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"},
            }),
        );
        rs.awaitReplication();
    }
    stopServerReplication(rs.getSecondaries());
}

// Stops replication at all shard secondaries.
export function stopReplicationOnSecondariesOfAllShards(st) {
    // The default WC is majority and this test can't satisfy majority writes.
    assert.commandWorked(
        st.s.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );
    st._rsObjects.forEach((rs) => stopReplicationOnSecondaries(rs, false));
}

// Restarts replication on the given server(s).
export function restartServerReplication(conn) {
    _toggleReplicationStall(conn, false);
}

// Restarts replication at all nodes in a replicaset.
export function restartReplSetReplication(rs) {
    restartServerReplication(rs.nodes);
}

// Restarts replication at all replicaset secondaries.
export function restartReplicationOnSecondaries(rs) {
    restartServerReplication(rs.getSecondaries());
}

// Restarts replication at all nodes in a sharded cluster.
export function restartReplicationOnAllShards(st) {
    st._rsObjects.forEach(restartReplSetReplication);
    restartReplSetReplication(st.configRS);
}

// Asserts that a writeConcernError was received.
export function assertWriteConcernError(res) {
    assert(res.writeConcernError, "No writeConcernError received, got: " + tojson(res));
    assert(res.writeConcernError.code, "No writeConcernError code, got: " + tojson(res));
    assert(res.writeConcernError.errmsg, "No writeConcernError errmsg, got: " + tojson(res));
}

// Run the specified command, on the admin database if specified.
export function runCommandCheckAdmin(db, cmd) {
    if (cmd.admin) {
        return db.adminCommand(cmd.req);
    } else {
        return db.runCommand(cmd.req);
    }
}

// Asserts that writeConcern timed out.
export function checkWriteConcernTimedOut(res) {
    assertWriteConcernError(res);
    const errInfo = res.writeConcernError.errInfo;
    assert(errInfo, "No writeConcernError errInfo, got: " + tojson(res));
    assert(errInfo.wtimeout, "No errInfo wtimeout, got: " + tojson(res));
}

/**
 * Tests that a command properly waits for writeConcern on retry. Takes an optional
 * 'setupFunc' that sets up the database state. 'setupFunc' accepts a connection to the
 * primary.
 */
export function runWriteConcernRetryabilityTest(priConn, secConn, cmd, kNodes, dbName, setupFunc) {
    dbName = dbName || "test";
    jsTestLog(`Testing ${tojson(cmd)} on ${dbName}.`);

    // The default WC is majority and stopServerReplication will prevent the replica set from
    // fulfilling any majority writes
    assert.commandWorked(
        priConn.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );

    // Send a dummy write to this connection so it will have the Client object initialized.
    const secondPriConn = new Mongo(priConn.host);
    const testDB2 = secondPriConn.getDB(dbName);
    assert.commandWorked(testDB2.dummy.insert({x: 1}, {writeConcern: {w: kNodes}}));

    if (setupFunc) {
        setupFunc(priConn);
    }

    stopServerReplication(secConn);

    const testDB = priConn.getDB(dbName);
    checkWriteConcernTimedOut(testDB.runCommand(cmd));

    // Retry the command on the new connection whose lastOp will be less than the main connection.
    checkWriteConcernTimedOut(testDB2.runCommand(cmd));

    // Retry the command on the main connection whose lastOp will not have changed.
    checkWriteConcernTimedOut(testDB.runCommand(cmd));

    // Bump forward the client lastOp on both connections and try again on both.
    assert.commandWorked(testDB.dummy.insert({x: 2}));
    assert.commandWorked(testDB2.dummy.insert({x: 3}));
    checkWriteConcernTimedOut(testDB.runCommand(cmd));
    checkWriteConcernTimedOut(testDB2.runCommand(cmd));

    restartServerReplication(secConn);
}
