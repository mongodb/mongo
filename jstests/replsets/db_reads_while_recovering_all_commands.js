/**
 * This file defines tests for all existing commands and their expected behavior when run against a
 * node that is in RECOVERING state.
 *
 * Tagged as multiversion-incompatible as the list of commands will vary depeding on version.
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

// This will verify the completeness of our map and run all tests.
load("jstests/libs/all_commands_test.js");

const name = jsTestName();
const dbName = "alltestsdb";
const collName = "alltestscoll";
const fullNs = dbName + "." + collName;

// Pre-written reasons for skipping a test.
const isAnInternalCommand = "internal command";
const isNotAUserDataRead = "does not return user data";
const isPrimaryOnly = "primary only";

const allCommands = {
    _addShard: {skip: isPrimaryOnly},
    _cloneCollectionOptionsFromPrimaryShard: {skip: isPrimaryOnly},
    _configsvrAddShard: {skip: isPrimaryOnly},
    _configsvrAddShardToZone: {skip: isPrimaryOnly},
    _configsvrBalancerCollectionStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStart: {skip: isPrimaryOnly},
    _configsvrBalancerStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStop: {skip: isPrimaryOnly},
    _configsvrClearJumboFlag: {skip: isPrimaryOnly},
    _configsvrCommitChunkMerge: {skip: isPrimaryOnly},
    _configsvrCommitChunkMigration: {skip: isPrimaryOnly},
    _configsvrCommitChunkSplit: {skip: isPrimaryOnly},
    _configsvrCommitMovePrimary: {skip: isPrimaryOnly},
    _configsvrCreateCollection: {skip: isPrimaryOnly},
    _configsvrCreateDatabase: {skip: isPrimaryOnly},
    _configsvrDropCollection: {skip: isPrimaryOnly},
    _configsvrDropDatabase: {skip: isPrimaryOnly},
    _configsvrEnableSharding: {skip: isPrimaryOnly},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isPrimaryOnly},
    _configsvrMoveChunk: {skip: isPrimaryOnly},
    _configsvrMovePrimary: {skip: isPrimaryOnly},
    _configsvrRefineCollectionShardKey: {skip: isPrimaryOnly},
    _configsvrRemoveShard: {skip: isPrimaryOnly},
    _configsvrRemoveShardFromZone: {skip: isPrimaryOnly},
    _configsvrShardCollection: {skip: isPrimaryOnly},
    _configsvrUpdateZoneKeyRange: {skip: isPrimaryOnly},
    _flushDatabaseCacheUpdates: {skip: isPrimaryOnly},
    _flushRoutingTableCacheUpdates: {skip: isPrimaryOnly},
    _getNextSessionMods: {skip: isPrimaryOnly},
    _getUserCacheGeneration: {skip: isNotAUserDataRead},
    _hashBSONElement: {skip: isNotAUserDataRead},
    _isSelf: {skip: isNotAUserDataRead},
    _killOperations: {skip: isNotAUserDataRead},
    _mergeAuthzCollections: {skip: isPrimaryOnly},
    _migrateClone: {skip: isPrimaryOnly},
    _recvChunkAbort: {skip: isPrimaryOnly},
    _recvChunkCommit: {skip: isPrimaryOnly},
    _recvChunkStart: {skip: isPrimaryOnly},
    _recvChunkStatus: {skip: isPrimaryOnly},
    _shardsvrCloneCatalogData: {skip: isPrimaryOnly},
    _shardsvrMovePrimary: {skip: isPrimaryOnly},
    _shardsvrShardCollection: {skip: isPrimaryOnly},
    _transferMods: {skip: isPrimaryOnly},
    abortTransaction: {skip: isPrimaryOnly},
    aggregate: {
        command: {aggregate: collName, pipeline: [{$match: {}}], cursor: {}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    appendOplogNote: {skip: isPrimaryOnly},
    applyOps: {skip: isPrimaryOnly},
    authenticate: {skip: isNotAUserDataRead},
    availableQueryOptions: {skip: isNotAUserDataRead},
    buildInfo: {skip: isNotAUserDataRead},
    captrunc: {skip: isPrimaryOnly},
    checkShardingIndex: {skip: isPrimaryOnly},
    cleanupOrphaned: {skip: isPrimaryOnly},
    clearLog: {skip: isNotAUserDataRead},
    cloneCollectionAsCapped: {skip: isPrimaryOnly},
    collMod: {skip: isPrimaryOnly},
    collStats: {
        command: {aggregate: collName, pipeline: [{$collStats: {count: {}}}], cursor: {}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    commitTransaction: {skip: isPrimaryOnly},
    compact: {skip: isNotAUserDataRead},
    configureFailPoint: {skip: isNotAUserDataRead},
    connPoolStats: {skip: isNotAUserDataRead},
    connPoolSync: {skip: isNotAUserDataRead},
    connectionStatus: {skip: isNotAUserDataRead},
    convertToCapped: {skip: isPrimaryOnly},
    coordinateCommitTransaction: {skip: isNotAUserDataRead},
    count: {
        command: {count: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    cpuload: {skip: isNotAUserDataRead},
    create: {skip: isPrimaryOnly},
    createIndexes: {skip: isPrimaryOnly},
    createRole: {skip: isPrimaryOnly},
    createUser: {skip: isPrimaryOnly},
    currentOp: {skip: isNotAUserDataRead},
    dataSize: {
        command: {dataSize: fullNs},
    },
    dbCheck: {skip: isPrimaryOnly},
    dbHash: {
        command: {dbHash: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    dbStats: {
        command: {dbStats: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    delete: {skip: isPrimaryOnly},
    distinct: {
        command: {distinct: collName, key: "a"},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    driverOIDTest: {skip: isNotAUserDataRead},
    drop: {skip: isPrimaryOnly},
    dropAllRolesFromDatabase: {skip: isPrimaryOnly},
    dropAllUsersFromDatabase: {skip: isPrimaryOnly},
    dropConnections: {skip: isNotAUserDataRead},
    dropDatabase: {skip: isPrimaryOnly},
    dropIndexes: {skip: isPrimaryOnly},
    dropRole: {skip: isPrimaryOnly},
    dropUser: {skip: isPrimaryOnly},
    echo: {skip: isNotAUserDataRead},
    emptycapped: {skip: isPrimaryOnly},
    endSessions: {skip: isNotAUserDataRead},
    explain: {
        command: {count: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    features: {skip: isNotAUserDataRead},
    filemd5: {skip: isNotAUserDataRead},
    find: {
        command: {find: collName, filter: {a: 1}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    findAndModify: {skip: isPrimaryOnly},
    flushRouterConfig: {skip: isNotAUserDataRead},
    fsync: {skip: isNotAUserDataRead},
    fsyncUnlock: {skip: isNotAUserDataRead},
    geoSearch: {
        command: {
            geoSearch: collName,
            search: {},
            near: [-42, 42],
        },
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary
    },
    getCmdLineOpts: {skip: isNotAUserDataRead},
    getDatabaseVersion: {skip: isNotAUserDataRead},
    getDefaultRWConcern: {skip: isNotAUserDataRead},
    getDiagnosticData: {skip: isNotAUserDataRead},
    getFreeMonitoringStatus: {skip: isNotAUserDataRead},
    getLastError: {skip: isPrimaryOnly},
    getLog: {skip: isNotAUserDataRead},
    getMore: {
        command: {getMore: NumberLong(123), collection: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary
    },
    getParameter: {skip: isNotAUserDataRead},
    getShardMap: {skip: isNotAUserDataRead},
    getShardVersion: {skip: isPrimaryOnly},
    getnonce: {skip: isNotAUserDataRead},
    godinsert: {skip: isAnInternalCommand},
    grantPrivilegesToRole: {skip: isPrimaryOnly},
    grantRolesToRole: {skip: isPrimaryOnly},
    grantRolesToUser: {skip: isPrimaryOnly},
    hostInfo: {skip: isNotAUserDataRead},
    httpClientRequest: {skip: isNotAUserDataRead},
    insert: {skip: isPrimaryOnly},
    internalRenameIfOptionsAndIndexesMatch: {skip: isAnInternalCommand},
    invalidateUserCache: {skip: isNotAUserDataRead},
    isMaster: {skip: isNotAUserDataRead},
    killAllSessions: {skip: isNotAUserDataRead},
    killAllSessionsByPattern: {skip: isNotAUserDataRead},
    killCursors: {skip: isNotAUserDataRead},
    killOp: {skip: isNotAUserDataRead},
    killSessions: {skip: isNotAUserDataRead},
    listCollections: {
        command: {listCollections: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary
    },
    listCommands: {command: {listCommands: 1}},
    listDatabases: {
        command: {listDatabases: 1},
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary
    },
    listIndexes: {
        command: {listIndexes: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary
    },
    lockInfo: {skip: isPrimaryOnly},
    logApplicationMessage: {skip: isNotAUserDataRead},
    logRotate: {skip: isNotAUserDataRead},
    logout: {skip: isNotAUserDataRead},
    makeSnapshot: {skip: isNotAUserDataRead},
    mapReduce: {
        command: {
            mapReduce: collName,
            map: function() {},
            reduce: function(key, vals) {},
            out: {inline: 1}
        },
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotMasterOrSecondary,
    },
    "mapreduce.shardedfinish": {skip: isAnInternalCommand},
    mergeChunks: {skip: isPrimaryOnly},
    moveChunk: {skip: isPrimaryOnly},
    ping: {skip: isNotAUserDataRead},
    planCacheClear: {skip: isNotAUserDataRead},
    planCacheClearFilters: {skip: isNotAUserDataRead},
    planCacheListFilters: {skip: isNotAUserDataRead},
    planCacheSetFilter: {skip: isNotAUserDataRead},
    prepareTransaction: {skip: isPrimaryOnly},
    profile: {skip: isPrimaryOnly},
    reapLogicalSessionCacheNow: {skip: isNotAUserDataRead},
    refreshLogicalSessionCacheNow: {skip: isNotAUserDataRead},
    refreshSessions: {skip: isNotAUserDataRead},
    reIndex: {skip: isNotAUserDataRead},
    renameCollection: {skip: isPrimaryOnly},
    repairDatabase: {skip: isNotAUserDataRead},
    replSetAbortPrimaryCatchUp: {skip: isNotAUserDataRead},
    replSetFreeze: {skip: isNotAUserDataRead},
    replSetGetConfig: {skip: isNotAUserDataRead},
    replSetGetRBID: {skip: isNotAUserDataRead},
    replSetGetStatus: {skip: isNotAUserDataRead},
    replSetHeartbeat: {skip: isNotAUserDataRead},
    replSetInitiate: {skip: isNotAUserDataRead},
    replSetMaintenance: {skip: isNotAUserDataRead},
    replSetReconfig: {skip: isNotAUserDataRead},
    replSetRequestVotes: {skip: isNotAUserDataRead},
    replSetStepDown: {skip: isNotAUserDataRead},
    replSetStepUp: {skip: isNotAUserDataRead},
    replSetSyncFrom: {skip: isNotAUserDataRead},
    replSetTest: {skip: isNotAUserDataRead},
    replSetUpdatePosition: {skip: isNotAUserDataRead},
    replSetResizeOplog: {skip: isNotAUserDataRead},
    resetError: {skip: isNotAUserDataRead},
    revokePrivilegesFromRole: {skip: isPrimaryOnly},
    revokeRolesFromRole: {skip: isPrimaryOnly},
    revokeRolesFromUser: {skip: isPrimaryOnly},
    rolesInfo: {skip: isPrimaryOnly},
    saslContinue: {skip: isPrimaryOnly},
    saslStart: {skip: isPrimaryOnly},
    serverStatus: {skip: isNotAUserDataRead},
    setCommittedSnapshot: {skip: isNotAUserDataRead},
    setDefaultRWConcern: {skip: isPrimaryOnly},
    setIndexCommitQuorum: {skip: isPrimaryOnly},
    setFeatureCompatibilityVersion: {skip: isPrimaryOnly},
    setFreeMonitoring: {skip: isPrimaryOnly},
    setParameter: {skip: isNotAUserDataRead},
    setShardVersion: {skip: isNotAUserDataRead},
    shardConnPoolStats: {skip: isNotAUserDataRead},
    shardingState: {skip: isNotAUserDataRead},
    shutdown: {skip: isNotAUserDataRead},
    sleep: {skip: isNotAUserDataRead},
    splitChunk: {skip: isPrimaryOnly},
    splitVector: {skip: isPrimaryOnly},
    stageDebug: {skip: isPrimaryOnly},
    startRecordingTraffic: {skip: isNotAUserDataRead},
    startSession: {skip: isNotAUserDataRead},
    stopRecordingTraffic: {skip: isNotAUserDataRead},
    top: {skip: isNotAUserDataRead},
    unsetSharding: {skip: isNotAUserDataRead},
    update: {skip: isPrimaryOnly},
    updateRole: {skip: isPrimaryOnly},
    updateUser: {skip: isPrimaryOnly},
    usersInfo: {skip: isPrimaryOnly},
    validate: {skip: isNotAUserDataRead},
    voteCommitIndexBuild: {skip: isNotAUserDataRead},
    waitForFailPoint: {skip: isNotAUserDataRead},
    waitForOngoingChunkSplits: {skip: isNotAUserDataRead},
    whatsmysni: {skip: isNotAUserDataRead},
    whatsmyuri: {skip: isNotAUserDataRead}
};

/**
 * Helper function for failing commands or writes that checks the result 'res' of either.
 * If 'code' is null we only check for failure, otherwise we confirm error code matches as
 * well. On assert 'msg' is printed.
 */
let assertCommandOrWriteFailed = function(res, code, msg) {
    if (res.writeErrors !== undefined) {
        assert.neq(0, res.writeErrors.length, msg);
    } else if (res.code !== null) {
        assert.commandFailedWithCode(res, code, msg);
    } else {
        assert.commandFailed(res, msg);
    }
};

// Set up a two-node replica set and put the secondary into RECOVERING state.
const rst = new ReplSetTest({name: name, nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
assert.commandWorked(primaryDb.getCollection(collName).insert({a: 42}));
rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryDb = secondary.getDB(dbName);

// This will lock the node into RECOVERING state until we turn it off.
assert.commandWorked(secondary.adminCommand({replSetMaintenance: 1}));

// Run all tests against the RECOVERING node.
AllCommandsTest.testAllCommands(secondary, allCommands, function(test) {
    const testDb = secondaryDb.getSiblingDB("test");
    let cmdDb = testDb;

    if (test.isAdminCommand) {
        cmdDb = testDb.getSiblingDB("admin");
    }

    if (test.expectFailure) {
        const expectedErrorCode = test.expectedErrorCode;
        assertCommandOrWriteFailed(
            cmdDb.runCommand(test.command), expectedErrorCode, () => tojson(test.command));
    } else {
        assert.commandWorked(cmdDb.runCommand(test.command), () => tojson(test.command));
    }
});

// Turn off maintenance mode and stop the test.
assert.commandWorked(secondary.adminCommand({replSetMaintenance: 0}));
rst.stopSet();
})();