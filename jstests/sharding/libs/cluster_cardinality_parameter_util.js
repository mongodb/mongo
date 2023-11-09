/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" on the given replica
 * set has the expected value.
 */
export function checkClusterParameter(rst, expectedValue) {
    let res = assert.commandWorked(rst.getPrimary().adminCommand(
        {getClusterParameter: "shardedClusterCardinalityForDirectConns"}));
    assert.eq(res.clusterParameters[0].hasTwoOrMoreShards, expectedValue);
}

/**
 * Interrupts the command for the command with the given name if it is running on the given node.
 */
export function interruptAdminCommand(node, cmdName) {
    const adminDB = node.getDB("admin");
    const results =
        adminDB
            .aggregate([{$currentOp: {}}, {$match: {["command." + cmdName]: {$exists: true}}}],
                       {$readPreference: {mode: "primaryPreferred"}})  // specify secondary ok.
            .toArray();
    if (results.length > 0) {
        adminDB.killOp(results[0].opid);
    }
}

export function interruptConfigsvrAddShard(configPrimary) {
    interruptAdminCommand(configPrimary, "_configsvrAddShard");
}

export function interruptConfigsvrRemoveShard(configPrimary) {
    interruptAdminCommand(configPrimary, "_configsvrRemoveShard");
}
