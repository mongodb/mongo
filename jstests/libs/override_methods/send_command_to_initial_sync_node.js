/**
 * Overrides runCommand to send the command both to the primary and to the initial sync node as
 * well.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

function getConn(connStr) {
    try {
        return new Mongo(connStr);
    } catch (exp) {
        jsTest.log('Unable to connect to ' + connStr + ": " + tojson(exp));
        return null;
    }
}

function sendCommandToInitialSyncNodeInReplSet(
    conn, _commandName, commandObj, func, makeFuncArgs, rsType) {
    const replSetStatus = conn.adminCommand({replSetGetStatus: 1});
    for (let i = 0; i < replSetStatus.members.length; i++) {
        const member = replSetStatus.members[i];
        if (member.stateStr == "STARTUP" || member.stateStr == "STARTUP2") {
            const initialSyncConn = getConn(member.name);
            if (initialSyncConn != null) {
                // Best effort attempt to send command to initial sync node. If it fails, move
                // on.
                try {
                    jsTestLog("Attempting to forward command to " + rsType +
                              " initial sync node: " + _commandName);
                    func.apply(initialSyncConn, makeFuncArgs(commandObj));
                } catch (exp) {
                    jsTest.log("Unable to apply command " + _commandName + ": " +
                               tojson(commandObj) + " on initial sync node: " + tojson(exp));
                } finally {
                    initialSyncConn.close();
                }
            }  // Move on if we can't get a connection to the node.
        }
    }
}

function maybeSendCommandToInitialSyncNodes(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    // DiscoverTopology.findConnectedNodes and sendCommandToInitialSyncNodeInReplSet will send
    // hello/isMaster, replSetGetStatus, getShardMap, and listShards to `conn` (the primary or the
    // mongos) to discover the topology and find the initial sync node(s), and since runCommand is
    // overriden with maybeSendCommandToInitialSyncNodes, this would result in infinite recursion,
    // so we need to instead skip trying to send these commands to the initial sync node.
    if (_commandName == "isMaster" || _commandName == "hello" || _commandName == "ismaster" ||
        _commandName == "replSetGetStatus" || _commandName == "getShardMap" ||
        _commandName == "listShards") {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // Ignore getLog/waitForFailpoint to avoid waiting for a log
    // message or a failpoint to be hit on the initial sync node.
    // Ignore fsync to avoid locking the initial sync node without unlocking.
    if (_commandName == "getLog" || _commandName == "waitForFailPoint" || _commandName == "fsync" ||
        _commandName == "fsyncUnlock") {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
    let topology;
    try {
        topology = DiscoverTopology.findConnectedNodes(conn);
    } catch (exp) {
        jsTestLog("Unable to run findConnectedNodes: " + tojson(exp))
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // Find initial sync nodes to send command to.
    if (topology.type == Topology.kReplicaSet) {
        sendCommandToInitialSyncNodeInReplSet(
            conn, _commandName, commandObj, func, makeFuncArgs, "replica set");
    } else if (topology.type == Topology.kShardedCluster) {
        // Forward command to the initial sync node in the first shard in the set.
        const shards = Object.entries(topology.shards);
        const [shardName, shard] = shards[0];
        if (shard.type == Topology.kReplicaSet) {
            const shardPrimaryConn = getConn(shard.primary);
            if (shardPrimaryConn != null) {
                try {
                    sendCommandToInitialSyncNodeInReplSet(
                        shardPrimaryConn, _commandName, commandObj, func, makeFuncArgs, shardName);
                } catch (exp) {
                    jsTest.log("Unable to apply command " + _commandName + ": " +
                               tojson(commandObj) + " on " + shardName +
                               " initial sync node: " + tojson(exp));
                } finally {
                    shardPrimaryConn.close();
                }
            }  // Move on if we can't get a connection to the node.
        }
        if (topology.configsvr.type == Topology.kReplicaSet) {
            const configConn = getConn(topology.configsvr.primary);
            if (configConn != null) {
                try {
                    sendCommandToInitialSyncNodeInReplSet(
                        configConn, _commandName, commandObj, func, makeFuncArgs, "config server");
                } catch (exp) {
                    jsTest.log("Unable to apply command " + _commandName + ": " +
                               tojson(commandObj) + " on config initial sync node: " + tojson(exp));

                } finally {
                    configConn.close();
                }
            }  // Move on if we can't get a connection to the node.
        }
    }

    // Finally, send the command as normal to the primary/mongos.
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/send_command_to_initial_sync_node.js");

OverrideHelpers.overrideRunCommand(maybeSendCommandToInitialSyncNodes);
