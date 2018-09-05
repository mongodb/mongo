const kSnapshotErrors =
    [ErrorCodes.SnapshotTooOld, ErrorCodes.SnapshotUnavailable, ErrorCodes.StaleChunkHistory];

function setFailCommandOnShards(st, mode, commands, code, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        assert.commandWorked(shardConn.adminCommand({
            configureFailPoint: "failCommand",
            mode: mode,
            data: {errorCode: code, failCommands: commands}
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
