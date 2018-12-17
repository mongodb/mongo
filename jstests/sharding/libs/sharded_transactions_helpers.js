const kSnapshotErrors =
    [ErrorCodes.SnapshotTooOld, ErrorCodes.SnapshotUnavailable, ErrorCodes.StaleChunkHistory];

function setFailCommandOnShards(st, mode, commands, code, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to
        // be an internal client.
        assert.commandWorked(shardConn.adminCommand({
            configureFailPoint: "failCommand",
            mode: mode,
            data: {errorCode: code, failCommands: commands, failInternalCommands: true}
        }));
    }
}

function unsetFailCommandOnEachShard(st, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        assert.commandWorked(
            shardConn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    }
}

function assertNoSuchTransactionOnAllShards(st, lsid, txnNumber) {
    st._rs.forEach(function(rs) {
        assertNoSuchTransactionOnConn(rs.test.getPrimary(), lsid, txnNumber);
    });
}

function assertNoSuchTransactionOnConn(conn, lsid, txnNumber) {
    assert.commandFailedWithCode(conn.getDB("foo").runCommand({
        find: "bar",
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction,
                                 "expected there to be no active transaction on shard, lsid: " +
                                     tojson(lsid) + ", txnNumber: " + tojson(txnNumber) +
                                     ", connection: " + tojson(conn));
}

function waitForFailpoint(hitFailpointStr, numTimes) {
    assert.soon(function() {
        const re = new RegExp(hitFailpointStr, 'g' /* find all occurrences */);
        return (rawMongoProgramOutput().match(re) || []).length == numTimes;
    }, 'Failed to find "' + hitFailpointStr + '" logged ' + numTimes + ' times');
}
