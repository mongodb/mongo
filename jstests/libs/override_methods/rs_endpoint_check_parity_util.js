const readCommandNames = new Set([
    "aggregate",
    "collStats",
    "count",
    "dbStats",
    "distinct",
    "find",
]);

/**
 * Throws an error if this command should not be run when testing replica set endpoint.
 */
function checkCanRun(dbName, commandName, commandObj) {
    if (commandName == "setFeatureCompatibilityVersion" && commandObj[commandName] != latestFCV) {
        throw Error("Cannot downgrade the FCV since the replica set endpoint is only " +
                    "supported in latest FCV");
    }
    if (commandName === "transitionToDedicatedConfigServer" || commandName === "addShard") {
        throw Error("Cannot run " + commandName + " command since the replica set endpoint " +
                    "is only supported on a single-shard cluster with embedded config servers");
    }
    if (dbName == "config" && commandObj[commandName] == "shards" &&
        !readCommandNames.has(commandName)) {
        throw Error("Cannot write to the config.shards collection since that could change the " +
                    "cluster topology and the replica set endpoint is only supported on " +
                    "single-shard cluster with embedded config servers");
    }
    if (commandName == "setParameter" &&
        commandObj.hasOwnProperty("internalQueryTopNAccumulatorBytes")) {
        throw Error("Cannot set 'internalQueryTopNAccumulatorBytes' to too low since it could " +
                    "cause the CheckRoutingTableConsistency hook to fail");
    }
    if (commandName === "getClusterParameter" || commandName == "auditConfigure" ||
        commandName == "getAuditConfig") {
        // TODO (SERVER-83421): Support cluster getClusterParameter command in embedded router.
        throw Error("Cannot run getClusterParameter command since it is currently not supported " +
                    "on embedded routers");
    }
    if (commandName === 'cloneCollectionAsCapped' || commandName === 'convertToCapped') {
        // TODO (SERVER-86360): Converting an unsharded collection to capped leads to inconsistent
        // metadata when featureFlagTrackUnshardedCollectionsUponCreation is enabled.
        throw Error("Cannot run " + commandName + " command since currently running it would lead" +
                    "to inconsistent metadata and cause the CheckMetadataConsistencyInBackground " +
                    "hook to fail");
    }
    if (typeof commandObj[commandName] === "string" &&
        commandObj[commandName].includes("system.resharding.") &&
        !readCommandNames.has(commandName)) {
        // TODO (SERVER-86487): Writing to system.resharding.* collection is allowed when
        // featureFlagTrackUnshardedCollectionsUponCreation is enabled and leads to
        // incomplete placement metadata.
        throw Error("Cannot write to a resharding temporary collection since it would result in " +
                    "a collection with incomplete placement metadata");
    }
}

/**
 * Runs the given command against both connections and compare the responses.
 */
export function runCommandCompareResponsesBase(
    conn0, conn1, dbName, commandName, commandObj, func, makeFuncArgs) {
    print("Comparing responses for command with dbName: " + dbName +
          ", cmdObj: " + JSON.stringify(commandObj));
    checkCanRun(dbName, commandName, commandObj);

    let resObj0, err0;
    try {
        resObj0 = func.apply(conn0, makeFuncArgs(commandObj));
    } catch (err) {
        err0 = err;
    }
    let resObj1, err1;
    try {
        resObj1 = func.apply(conn1, makeFuncArgs(commandObj));
    } catch (err) {
        err1 = err;
    }

    if (err0) {
        throw err0;
    }
    // TODO (SERVER-86834): Add response comparison to replica set endpoint jstestfuzz suites.
    return resObj0;
}
