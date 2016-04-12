/**
 * Utilities for testing writeConcern.
 */

// Shards a collection and creates 2 chunks, one on each s of two shards.
function shardCollectionWithChunks(st, coll) {
    var _db = coll.getDB();
    var numberDoc = 20;
    coll.ensureIndex({x: 1}, {unique: true});
    st.ensurePrimaryShard(_db.toString(), st._shardNames[0]);
    st.shardColl(
        coll.getName(), {x: 1}, {x: numberDoc / 2}, {x: numberDoc / 2}, _db.toString(), true);

    for (var i = 0; i < numberDoc; i++) {
        coll.insert({x: i});
    }
    assert.eq(coll.count(), numberDoc);
}

// Stops replication at a server.
function stopServerReplication(conn) {
    conn.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});
}

// Stops replication at all replicaset secondaries.
function stopReplicationOnSecondaries(rs) {
    var secondaries = rs.getSecondaries();
    secondaries.forEach(stopServerReplication);
}

// Stops replication at all shard secondaries.
function stopReplicationOnSecondariesOfAllShards(st) {
    st._rsObjects.forEach(stopReplicationOnSecondaries);
}

// Restarts replication at a server.
function restartServerReplication(conn) {
    conn.getDB('admin').runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
}

// Restarts replication at all nodes in a replicaset.
function restartReplSetReplication(rs) {
    rs.nodes.forEach(restartServerReplication);
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
