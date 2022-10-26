/**
 * Overrides runCommand to use the $_passthroughToShard parameter. The changestreams per-shard
 * cursor passthrough suite ensures changestream tests can still run correctly on a single-shard
 * cluster. By adding this parameter, we pass through to that single shard, running the pipelines
 * directly on that mongod. This will test the machinery of per-shard cursors via mongos.
 */

(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/discover_topology.js");                  // For 'DiscoverTopology'.

// To be eligible, a command must be a changeStream request sent to a mongos.
const isEligibleForPerShardCursor = function(conn, cmdObj) {
    if (!(cmdObj && cmdObj.aggregate && Array.isArray(cmdObj.pipeline) &&
          cmdObj.pipeline.length > 0 && typeof cmdObj.pipeline[0].$changeStream == "object" &&
          cmdObj.pipeline[0].$changeStream.constructor === Object)) {
        return false;
    }
    return conn.isMongos();
};

const discoverShardId = function(conn) {
    const topology = DiscoverTopology.findConnectedNodes(conn);
    const shards = topology.shards;
    let shardName = Object.keys(shards)[0];
    return {shard: shardName};
};

function runCommandWithPassthroughToShard(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    if (!isEligibleForPerShardCursor(conn, commandObj)) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    commandObj.$_passthroughToShard = discoverShardId(conn);
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicit_passthrough_to_shard_changestreams.js");

OverrideHelpers.overrideRunCommand(runCommandWithPassthroughToShard);
}());
