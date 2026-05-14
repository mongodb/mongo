import {ShardingTest} from "jstests/libs/shardingtest.js";

// Make it easier to understand whether or not returns from the assert.soon are being retried.
const kRetry = false;
const kNoRetry = true;

export function removeShard(shardingTestOrConn, shardName, timeout) {
    if (timeout == undefined) {
        timeout = 10 * 60 * 1000; // 10 minutes
    }

    let s;
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
    // adds random choice to use new API or old API
    if (is_83 && Math.random() > 0.5) {
        removeShardNew(s, shardName, timeout);
    } else {
        removeShardOld(s, shardName, timeout);
    }
}

function removeShardOld(s, shardName, timeout) {
    assert.soon(
        function () {
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
            return res.state == "completed";
        },
        "failed to remove shard " + shardName + " within " + timeout + "ms",
        timeout,
    );
}

/**
 * Removes a shard using the new three-phase protocol (MongoDB 8.3+).
 * Phases: start → draining → commit
 */
function removeShardNew(s, shardName, timeout) {
    const isConfigShard = shardName === "config";
    const commands = isConfigShard
        ? {
              start: {startTransitionToDedicatedConfigServer: 1},
              status: {getTransitionToDedicatedConfigServerStatus: 1},
              commit: {commitTransitionToDedicatedConfigServer: 1},
              phaseName: "config server transition",
          }
        : {
              start: {startShardDraining: shardName},
              status: {shardDrainingStatus: shardName},
              commit: {commitShardRemoval: shardName},
              phaseName: "shard removal",
          };

    let phase = "start";

    assert.soon(
        function () {
            switch (phase) {
                case "start": {
                    const result = retryIfRetriableError(s, commands.start);
                    if (result.success) {
                        phase = "draining";
                        return kRetry;
                    }
                    return result.shouldRetry ? kRetry : kNoRetry;
                }
                case "draining": {
                    const result = retryIfRetriableError(s, commands.status);
                    if (!result.success) {
                        return result.shouldRetry ? kRetry : kNoRetry;
                    }
                    if (result.response.state === "drainingComplete") {
                        phase = "commit";
                    }
                    return kRetry;
                }
                case "commit": {
                    const result = retryIfRetriableError(s, commands.commit, true);
                    if (result.success) {
                        return kNoRetry;
                    }
                    if (result.shouldRetry) {
                        if (result.shouldRetryDraining) {
                            phase = "status";
                        }
                        return kRetry;
                    }
                    throw new Error(`Unexpected error during ${phase}: ${result.response}`);
                }
                default:
                    throw new Error(`Unknown phase: ${phase}`);
            }
        },
        `failed to remove shard ${shardName} within ${timeout} ms. Last phase: ${phase}`,
        timeout,
    );
}

/**
 * Executes a command and returns a result object with the following properties:
 * - success: boolean indicating if the command was successful
 * - shouldRetry: boolean indicating if the command should be retried
 * - response: the response from the command
 * @returns {Object} {success: boolean, shouldRetry: boolean, response: Object}
 */
function retryIfRetriableError(s, command, isDrainingErrorAllowed = false) {
    const res = s.adminCommand(command);
    if (!res.ok) {
        if (res.code === ErrorCodes.ShardNotFound) {
            // If shardNotFound don't retry to make the removeShard function idempotent.
            return {success: false, shouldRetry: false, response: res, shouldRetryDraining: false};
        }
        if (res.code === ErrorCodes.HostUnreachable && TestData.runningWithConfigStepdowns) {
            // The mongos may exhaust its retries due to having consecutive config
            // stepdowns. In this case, the mongos will return a HostUnreachable error.
            // We should retry the operation when this happens.
            return {success: false, shouldRetry: true, response: res, shouldRetryDraining: false};
        }
        if (
            isDrainingErrorAllowed &&
            res.code === ErrorCodes.IllegalOperation &&
            // It's possible that the shard is not completely drained even after the drainingComplete
            // status is returned. This can happen when a new unsplittable collection is created on the
            // draining shard, when for example a failpoint like
            // createUnshardedCollectionRandomizeDataShard places a collection on a random shard.
            (res.errmsg || "").includes("isn't completely drained")
        ) {
            return {success: false, shouldRetry: true, response: res, shouldRetryDraining: true};
        }
        return {success: false, shouldRetry: false, response: res, shouldRetryDraining: false};
    }

    assert.commandWorked(res);
    return {success: true, shouldRetry: false, response: res};
}

export function moveOutSessionChunks(st, fromShard, toShard) {
    const kSessionsColl = "config.system.sessions";
    let sessionCollEntry = st.s.getDB("config").collections.findOne({_id: kSessionsColl});

    st.s
        .getDB("config")
        .chunks.find({uuid: sessionCollEntry.uuid, shard: fromShard})
        .forEach((chunkEntry) => {
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: kSessionsColl,
                    find: chunkEntry.min,
                    to: toShard,
                    _waitForDelete: true,
                }),
            );
        });
}
