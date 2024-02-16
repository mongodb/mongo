import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

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
    if (commandName === "getClusterParameter" || commandName == "auditConfigure") {
        // TODO (SERVER-83421): Support cluster getClusterParameter command in embedded router.
        throw Error("Cannot run getClusterParameter command since it is currently not supported " +
                    "on embedded routers");
    }
    if (commandName === 'cloneCollectionAsCapped' || commandName === 'convertToCapped') {
        // TODO (SERVER-86360): Converting an unsharded collection to capped leads to inconsistent
        // metadata when featureFlagTrackUnshardedCollectionsOnShardingCatalog is enabled.
        throw Error("Cannot run " + commandName + " command since currently running it would lead" +
                    "to inconsistent metadata and cause the CheckMetadataConsistencyInBackground " +
                    "hook to fail");
    }
    if (typeof commandObj[commandName] === "string" &&
        commandObj[commandName].includes("system.resharding.") &&
        !readCommandNames.has(commandName)) {
        // TODO (SERVER-86487): Writing to system.resharding.* collection is allowed when
        // featureFlagTrackUnshardedCollectionsOnShardingCatalog is enabled and leads to
        // incomplete placement metadata.
        throw Error("Cannot write to a resharding temporary collection since it would result in " +
                    "a collection with incomplete placement metadata");
    }
}

function runCommand(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    print("Running command with dbName: " + dbName + ", cmdObj: " + JSON.stringify(commandObj));
    checkCanRun(dbName, commandName, commandObj);
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/rs_endpoint.js");

OverrideHelpers.overrideRunCommand(runCommand);
