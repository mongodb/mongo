/**
 * Utilities for testing writeConcern.
 */

load("jstests/libs/check_log.js");

// Shards a collection with 'numDocs' documents and creates 2 chunks, one on each of two shards.
function shardCollectionWithChunks(st, coll, numDocs) {
    var _db = coll.getDB();
    var numberDoc = numDocs || 20;
    coll.ensureIndex({x: 1}, {unique: true});
    st.ensurePrimaryShard(_db.toString(), st.shard0.shardName);
    st.shardColl(
        coll.getName(), {x: 1}, {x: numberDoc / 2}, {x: numberDoc / 2}, _db.toString(), true);

    for (var i = 0; i < numberDoc; i++) {
        coll.insert({x: i});
    }
    assert.eq(coll.count(), numberDoc);
}

// Stops replication on the given server(s).
function stopServerReplication(conn) {
    if (conn.length) {
        conn.forEach(function(n) {
            stopServerReplication(n);
        });
        return;
    }

    // Clear ramlog so checkLog can't find log messages from previous times this fail point was
    // enabled.
    assert.commandWorked(conn.adminCommand({clearLog: 'global'}));
    var errMsg = 'Failed to enable stopReplProducer failpoint.';
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: 'stopReplProducer', mode: 'alwaysOn'}), errMsg);

    // Wait until the fail point is actually hit.
    checkLog.contains(conn, 'bgsync - stopReplProducer fail point enabled');
}

// Stops replication at all replicaset secondaries.
function stopReplicationOnSecondaries(rs) {
    stopServerReplication(rs.getSecondaries());
}

// Stops replication at all shard secondaries.
function stopReplicationOnSecondariesOfAllShards(st) {
    st._rsObjects.forEach(stopReplicationOnSecondaries);
}

// Restarts replication on the given server(s).
function restartServerReplication(conn) {
    if (conn.length) {
        conn.forEach(function(n) {
            restartServerReplication(n);
        });
        return;
    }

    var errMsg = 'Failed to disable stopReplProducer failpoint.';
    assert.commandWorked(
        conn.getDB('admin').runCommand({configureFailPoint: 'stopReplProducer', mode: 'off'}),
        errMsg);
}

// Restarts replication at all nodes in a replicaset.
function restartReplSetReplication(rs) {
    restartServerReplication(rs.nodes);
}

// Restarts replication at all replicaset secondaries.
function restartReplicationOnSecondaries(rs) {
    restartServerReplication(rs.getSecondaries());
}

// Restarts replication at all nodes in a sharded cluster.
function restartReplicationOnAllShards(st) {
    st._rsObjects.forEach(restartReplSetReplication);
    restartReplSetReplication(st.configRS);
}

// Asserts that a writeConcernError was received.
function assertWriteConcernError(res) {
    assert(res.writeConcernError, "No writeConcernError received, got: " + tojson(res));
    assert(res.writeConcernError.code);
    assert(res.writeConcernError.errmsg);
}

// Run the specified command, on the admin database if specified.
function runCommandCheckAdmin(db, cmd) {
    if (cmd.admin) {
        return db.adminCommand(cmd.req);
    } else {
        return db.runCommand(cmd.req);
    }
}
