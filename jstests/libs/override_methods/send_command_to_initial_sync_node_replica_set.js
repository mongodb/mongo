/**
 * Overrides runCommand to send the command both to the primary and to the initial sync node as
 * well for replica sets.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    sendCommandToInitialSyncNodeInReplSet,
    shouldSkipCommand
} from "jstests/libs/override_methods/send_command_to_initial_sync_node_lib.js";

function maybeSendCommandToInitialSyncNodes(
    conn, _dbName, _commandName, commandObj, func, makeFuncArgs) {
    // Skip forwarding incompatible commands to initial sync node.
    if (shouldSkipCommand(conn, _commandName, commandObj, func, makeFuncArgs)) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    sendCommandToInitialSyncNodeInReplSet(
        conn, _commandName, commandObj, func, makeFuncArgs, "replica set");

    // Finally, send the command as normal to the primary/mongos.
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/send_command_to_initial_sync_node_replica_set.js");

OverrideHelpers.overrideRunCommand(maybeSendCommandToInitialSyncNodes);
