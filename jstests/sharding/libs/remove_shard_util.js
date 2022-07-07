function removeShard(st, shardName, timeout) {
    if (timeout == undefined) {
        timeout = 10 * 60 * 1000;  // 10 minutes
    }

    assert.soon(function() {
        let res = assert.commandWorked(st.s.adminCommand({removeShard: shardName}));
        if (!res.ok && res.code === ErrorCodes.ShardNotFound) {
            // If the config server primary steps down right after removing the config.shards doc
            // for the shard but before responding with "state": "completed", the mongos would retry
            // the _configsvrRemoveShard command against the new config server primary, which would
            // not find the removed shard in its ShardRegistry if it has done a ShardRegistry reload
            // after the config.shards doc for the shard was removed. This would cause the command
            // to fail with ShardNotFound.
            return true;
        }
        return res.state == 'completed';
    }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);
}
