/**
 * Overrides runCommand to send the command both to the primary and to the initial sync node as
 * well for sharded clusters.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    getConn,
    sendCommandToInitialSyncNodeInReplSet,
    shouldSkipCommand
} from "jstests/libs/override_methods/send_command_to_initial_sync_node_lib.js";

function maybeSendCommandToInitialSyncNodesShardedCluster(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    // Skip forwarding incompatible commands to initial sync node.
    if (shouldSkipCommand(conn, _commandName, commandObj, func, makeFuncArgs)) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // Forward command to initial sync node on one shard.
    const listShardsRes = conn.adminCommand({listShards: 1});
    if (!listShardsRes.ok) {
        jsTestLog("Unable to run listShards: " + tojson(listShardsRes) +
                  ", skipping forwarding command " + _commandName + " to initial sync node");
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (!listShardsRes.hasOwnProperty('shards')) {
        jsTestLog('Expected "listShards" command to return an object with a "shards" field: ' +
                  tojson(listShardsRes) + ", skipping forwarding command " + _commandName +
                  " to initial sync node");
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    // The passthrough suite using this should always have at least one shard.
    const shardInfo = listShardsRes.shards[0];
    const shardPrimaryConn = getConn(shardInfo.host);
    if (shardPrimaryConn != null) {
        try {
            sendCommandToInitialSyncNodeInReplSet(
                shardPrimaryConn, _commandName, commandObj, func, makeFuncArgs, "shard");
        } catch (exp) {
            jsTest.log("Unable to apply command " + _commandName + ": " + tojson(commandObj) +
                       " on shard initial sync node: " + tojson(exp));
        } finally {
            shardPrimaryConn.close();
        }

    }  // Move on if we can't get a connection to the node.

    // Try to forward command to config initial sync node.
    const shardMap = conn.adminCommand({getShardMap: 1});
    if (!shardMap.ok) {
        jsTestLog("Unable to run getShardMap: " + tojson(shardMap) +
                  ", skipping forwarding command " + _commandName + " to initial sync node")
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (!shardMap.hasOwnProperty('map')) {
        jsTestLog('Expected "getShardMap" command to return an object with a "map" field: ' +
                  tojson(shardMap) + ", skipping forwarding command " + _commandName +
                  " to initial sync node");
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (!shardMap.map.hasOwnProperty('config')) {
        jsTestLog('Expected "getShardMap" command to return an object with a "map.config"' +
                  ' field: ' + tojson(shardMap) + ", skipping forwarding command " + _commandName +
                  " to initial sync node");
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const configConn = getConn(shardMap.map.config);
    if (configConn != null) {
        try {
            sendCommandToInitialSyncNodeInReplSet(
                configConn, _commandName, commandObj, func, makeFuncArgs, "config server");
        } catch (exp) {
            jsTest.log("Unable to apply command " + _commandName + ": " + tojson(commandObj) +
                       " on config initial sync node: " + tojson(exp));

        } finally {
            configConn.close();
        }
    }  // Move on if we can't get a connection to the node.
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/send_command_to_initial_sync_node_sharded_cluster.js");

OverrideHelpers.overrideRunCommand(maybeSendCommandToInitialSyncNodesShardedCluster);
