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
 * Interrupts the commands with the given names if they are running on the given node.
 */
export function interruptAdminCommand(node, cmdNames) {
    const adminDB = node.getDB("admin");
    const cmdNameFilter = [];
    cmdNames.forEach(cmdName => {
        cmdNameFilter.push({["command." + cmdName]: {$exists: true}});
    });
    const results =
        adminDB
            .aggregate([{$currentOp: {}}, {$match: {$or: cmdNameFilter}}],
                       {$readPreference: {mode: "primaryPreferred"}})  // specify secondary ok.
            .toArray();
    results.forEach(result => {
        adminDB.killOp(result.opid);
    });
}

export function interruptConfigsvrAddShard(configPrimary) {
    interruptAdminCommand(configPrimary,
                          ["_configsvrAddShard", "_configsvrTransitionToDedicatedConfigServer"]);
}

export function interruptConfigsvrRemoveShard(configPrimary) {
    interruptAdminCommand(configPrimary,
                          ["_configsvrRemoveShard", "_configsvrTransitionToDedicatedConfigServer"]);
}
