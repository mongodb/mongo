/**
 * Helper functions for sending command to initial sync node.
 */
export function getConn(connStr) {
    try {
        return new Mongo(connStr);
    } catch (exp) {
        jsTest.log('Unable to connect to ' + connStr + ": " + tojson(exp));
        return null;
    }
}

export function shouldSkipCommand(_commandName, commandObj) {
    let skippedCommands = {
        // These commands are skipped because finding connected nodes and
        // sendCommandToInitialSyncNodeInReplSet will send
        // hello/isMaster, replSetGetStatus, getShardMap, and listShards to `conn` (the primary or
        // the mongos) to discover the topology and find the initial sync node(s), and since
        // runCommand is overriden with maybeSendCommandToInitialSyncNodes, this would result in
        // infinite recursion, so we need to instead skip trying to send these commands to the
        // initial sync node.
        "isMaster": true,
        "hello": true,
        "ismaster": true,
        "replSetGetStatus": true,
        "getShardMap": true,
        "listShards": true,
        // Ignore getLog/waitForFailpoint to avoid waiting for a log
        // message or a failpoint to be hit on the initial sync node.
        "getLog": true,
        "waitForFailPoint": true,
        "configureFailPoint": true,
        // Ignore fsync to avoid locking the initial sync node without unlocking.
        "fsync": true,
        "fsyncUnlock": true,
    };

    if (_commandName in skippedCommands) {
        return true;
    }

    if (typeof commandObj !== "object" || commandObj === null) {
        return true;
    }
    return false;
}

export function sendCommandToInitialSyncNodeInReplSet(
    conn, _commandName, commandObj, func, makeFuncArgs, rsType) {
    const replSetStatus = conn.adminCommand({replSetGetStatus: 1});
    assert.commandWorked(replSetStatus);

    // The passthrough suite running this should always have the initial sync node as the 3rd node
    // in the set.
    const initialSyncNode = replSetStatus.members[2];
    assert.eq(initialSyncNode.stateStr, "STARTUP2");

    const initialSyncConn = getConn(initialSyncNode.name);
    if (initialSyncConn != null) {
        // Best effort attempt to send command to initial sync node. If it fails, move
        // on.
        try {
            jsTestLog("Attempting to forward command to " + rsType +
                      " initial sync node: " + _commandName + " (this may end up being a no-op)");
            func.apply(initialSyncConn, makeFuncArgs(commandObj));
        } catch (exp) {
            jsTest.log("Unable to apply command " + _commandName + ": " + tojson(commandObj) +
                       " on initial sync node: " + tojson(exp));
        } finally {
            initialSyncConn.close();
        }
    }  // Move on if we can't get a connection to the node.
}
