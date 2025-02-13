import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function isConfigShardReplicaSet(adminDB) {
    assert(!FixtureHelpers.isMongos(adminDB));

    const configShardDoc = adminDB.getSiblingDB("config").shards.findOne({_id: "config"});
    if (configShardDoc == null) {
        return false;
    }
    const shardIdentityDoc = adminDB.system.version.findOne({_id: "shardIdentity"});
    if (shardIdentityDoc == null) {
        return false;
    }
    return shardIdentityDoc.shardName == "config";
}

export function isReplicaSet(conn) {
    const adminDB = conn.getDB("admin");
    return FixtureHelpers.isReplSet(adminDB) && !isConfigShardReplicaSet(adminDB);
}

export function isShardedClusterReplicaSetEndpoint(conn) {
    const adminDB = conn.getDB("admin");
    return FeatureFlagUtil.isEnabled(adminDB, "ReplicaSetEndpoint") &&
        FixtureHelpers.isReplSet(adminDB) && isConfigShardReplicaSet(adminDB);
}

export function isShardedClusterDedicatedRouter(conn) {
    const adminDB = conn.getDB("admin");
    return FixtureHelpers.isMongos(adminDB);
}

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
    if (commandName === 'cloneCollectionAsCapped') {
        // TODO (SERVER-80416): Support cloneCollectionAsCapped in sharded clusters.
        throw Error("Cannot run " + commandName + " command since currently running it would lead" +
                    "to inconsistent metadata and cause the CheckMetadataConsistencyInBackground " +
                    "hook to fail");
    }
}

/**
 * Runs the given command against both connections and compare the responses.
 */
export function runCommandCompareResponsesBase(
    conn0, conn1, dbName, commandName, commandObj, func, makeFuncArgs) {
    jsTest.log.info("Comparing responses for command with dbName: " + dbName, {commandObj});
    checkCanRun(dbName, commandName, commandObj);

    let resObj0, err0;
    try {
        resObj0 = func.apply(conn0, makeFuncArgs(commandObj));
    } catch (err) {
        err0 = err;
    }
    // TODO (SERVER-86834): Add response comparison to replica set endpoint jstestfuzz suites.
    // let resObj1, err1;
    // try {
    //     resObj1 = func.apply(conn1, makeFuncArgs(commandObj));
    // } catch (err) {
    //     err1 = err;
    // }

    if (err0) {
        throw err0;
    }
    // TODO (SERVER-86834): Add response comparison to replica set endpoint jstestfuzz suites.
    return resObj0;
}
