import {ShardingTest} from "jstests/libs/shardingtest.js";

export function removeShard(shardingTestOrConn, shardName, timeout) {
    if (timeout == undefined) {
        timeout = 10 * 60 * 1000;  // 10 minutes
    }

    var s;
    if (shardingTestOrConn instanceof ShardingTest) {
        s = shardingTestOrConn.s;
    } else {
        s = shardingTestOrConn;
    }

    assert.soon(function() {
        let res;
        if (shardName == "config") {
            // Need to use transitionToDedicatedConfigServer if trying
            // to remove config server as a shard
            res = s.adminCommand({transitionToDedicatedConfigServer: shardName});
        } else {
            res = s.adminCommand({removeShard: shardName});
        }
        if (!res.ok) {
            if (res.code === ErrorCodes.ShardNotFound) {
                // TODO SERVER-32553: Clarify whether we should handle this scenario in tests or
                // mongos If the config server primary steps down right after removing the
                // config.shards doc for the shard but before responding with "state": "completed",
                // the mongos would retry the _configsvrRemoveShard command against the new config
                // server primary, which would not find the removed shard in its ShardRegistry if it
                // has done a ShardRegistry reload after the config.shards doc for the shard was
                // removed. This would cause the command to fail with ShardNotFound.
                return true;
            }
            if (res.code === ErrorCodes.HostUnreachable && TestData.runningWithConfigStepdowns) {
                // The mongos may exhaust its retries due to having consecutive config stepdowns. In
                // this case, the mongos will return a HostUnreachable error.
                return true;
            }
        }
        assert.commandWorked(res);
        return res.state == 'completed';
    }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);
}

export function moveOutSessionChunks(st, fromShard, toShard) {
    const kSessionsColl = 'config.system.sessions';
    let sessionCollEntry = st.s.getDB('config').collections.findOne({_id: kSessionsColl});

    st.s.getDB('config')
        .chunks.find({uuid: sessionCollEntry.uuid, shard: fromShard})
        .forEach((chunkEntry) => {
            assert.commandWorked(st.s.adminCommand({
                moveChunk: kSessionsColl,
                find: chunkEntry.min,
                to: toShard,
                _waitForDelete: true
            }));
        });
}
