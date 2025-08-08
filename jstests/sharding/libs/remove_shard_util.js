import {ShardingTest} from "jstests/libs/shardingtest.js";

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kRetry = false;
const kNoRetry = true;

export function removeShard(shardingTestOrConn, shardName, timeout) {
    if (timeout == undefined) {
        timeout = 10 * 60 * 1000;  // 10 minutes
    }

    var s;
    let admin;
    if (shardingTestOrConn instanceof ShardingTest) {
        s = shardingTestOrConn.s;
        admin = s.getDB("admin");
    } else {
        s = shardingTestOrConn;
        admin = s.getSiblingDB("admin");
    }

    // TODO SERVER-97816 remove the removeShardOld function
    // Check the fcv. If 8.3 or above run the new path, otherwise run the old path.
    const res = admin.system.version.find({_id: "featureCompatibilityVersion"}).toArray();
    const is_83 = res.length == 0 || MongoRunner.compareBinVersions(res[0].version, "8.3") >= 0;
    if (is_83) {
        removeShardNew(s, shardName, timeout);
    } else {
        removeShardOld(s, shardName, timeout);
    }
}

function removeShardOld(s, shardName, timeout) {
    assert.soon(function() {
        let res;
        if (shardName == "config") {
            // Need to use transitionToDedicatedConfigServer if trying
            // to remove config server as a shard
            res = s.adminCommand({transitionToDedicatedConfigServer: shardName});
        } else {
            res = s.adminCommand({removeShard: shardName});
        }

        // TODO (SERVER-97816): remove multiversion check
        const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);
        if (!res.ok) {
            if (isMultiversion && res.code === ErrorCodes.ShardNotFound) {
                // TODO SERVER-32553: Clarify whether we should handle this scenario in tests or
                // mongos If the config server primary steps down right after removing the
                // config.shards doc for the shard but before responding with "state": "completed",
                // the mongos would retry the _configsvrRemoveShard command against the new config
                // server primary, which would not find the removed shard in its ShardRegistry if it
                // has done a ShardRegistry reload after the config.shards doc for the shard was
                // removed. This would cause the command to fail with ShardNotFound.
                return kNoRetry;
            }
            if (res.code === ErrorCodes.HostUnreachable && TestData.runningWithConfigStepdowns) {
                // The mongos may exhaust its retries due to having consecutive config stepdowns. In
                // this case, the mongos will return a HostUnreachable error.
                // We should retry the operation when this happens.
                return kRetry;
            }
        }
        assert.commandWorked(res);
        return res.state == 'completed';
    }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);
}

function removeShardNew(s, shardName, timeout) {
    let res;
    if (shardName == "config") {
        assert.soon(function() {
            // Need to use transitionToDedicatedConfigServer if trying
            // to remove config server as a shard
            res = s.adminCommand({transitionToDedicatedConfigServer: shardName});
            if (!res.ok) {
                if (res.code == ErrorCodes.ShardNotFound) {
                    // TODO SERVER-32553: same as above
                    return kNoRetry;
                }
                if (res.code === ErrorCodes.HostUnreachable &&
                    TestData.runningWithConfigStepdowns) {
                    // The mongos may exhaust its retries due to having consecutive config
                    // stepdowns. In this case, the mongos will return a HostUnreachable error.
                    // We should retry the operation when this happens.
                    return kRetry;
                }
            }
            assert.commandWorked(res);
            return res.state == 'completed';
        }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);
    } else {
        assert.soon(function() {
            assert.soon(function() {
                return retryIfRetriableError(s, {startShardDraining: shardName});
            }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);

            assert.soon(function() {
                return retryIfRetriableError(s, {shardDrainingStatus: shardName});
            }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);

            res = s.adminCommand({commitShardRemoval: shardName});

            if (!res.ok) {
                // If commitShardRemoval fails for any reason, retry.
                return kRetry;
            }
            assert.commandWorked(res);
            return true;
        }, "failed to remove shard " + shardName + " within " + timeout + "ms", timeout);
    }
}
function retryIfRetriableError(s, command) {
    let res = s.adminCommand(command);
    if (!res.ok) {
        if (res.code == ErrorCodes.ShardNotFound) {
            // If shardNotFound don't retry to make the removeShard function idempotent.
            return kNoRetry;
        }
        if (res.code === ErrorCodes.HostUnreachable && TestData.runningWithConfigStepdowns) {
            // The mongos may exhaust its retries due to having consecutive config
            // stepdowns. In this case, the mongos will return a HostUnreachable error.
            // We should retry the operation when this happens.
            return kRetry;
        }
    }
    assert.commandWorked(res);
    return res.state ? res.state == 'drainingComplete' : kNoRetry;
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
