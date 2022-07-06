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
    _configsvrAbortReshardCollection: {skip: isPrimaryOnly},
    _configsvrAddShard: {skip: isPrimaryOnly},
    _configsvrAddShardToZone: {skip: isPrimaryOnly},
    _configsvrBalancerCollectionStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStart: {skip: isPrimaryOnly},
    _configsvrBalancerStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStop: {skip: isPrimaryOnly},
    _configsvrCleanupReshardCollection: {skip: isPrimaryOnly},
    _configsvrCollMod: {skip: isAnInternalCommand},
    _configsvrClearJumboFlag: {skip: isPrimaryOnly},
    _configsvrCommitChunksMerge: {skip: isPrimaryOnly},
    _configsvrCommitChunkMigration: {skip: isPrimaryOnly},
    _configsvrCommitChunkSplit: {skip: isPrimaryOnly},
    _configsvrCommitReshardCollection: {skip: isPrimaryOnly},
    _configsvrConfigureCollectionBalancing: {skip: isPrimaryOnly},
    _configsvrCreateDatabase: {skip: isPrimaryOnly},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isPrimaryOnly},
    _configsvrMoveChunk: {skip: isPrimaryOnly},
    _configsvrMoveRange: {skip: isPrimaryOnly},
    _configsvrRefineCollectionShardKey: {skip: isPrimaryOnly},
    _configsvrRemoveChunks: {skip: isPrimaryOnly},
    _configsvrRemoveShard: {skip: isPrimaryOnly},
    _configsvrRemoveShardFromZone: {skip: isPrimaryOnly},
    _configsvrRemoveTags: {skip: isPrimaryOnly},
    _configsvrRepairShardedCollectionChunksHistory: {skip: isPrimaryOnly},
    _configsvrRenameCollectionMetadata: {skip: isPrimaryOnly},
    _configsvrReshardCollection: {skip: isPrimaryOnly},
    _configsvrRunRestore: {skip: isPrimaryOnly},
    _configsvrSetAllowMigrations: {skip: isPrimaryOnly},
    _configsvrSetClusterParameter: {skip: isPrimaryOnly},
    _configsvrSetUserWriteBlockMode: {skip: isPrimaryOnly},
    _configsvrUpdateZoneKeyRange: {skip: isPrimaryOnly},
    _flushDatabaseCacheUpdates: {skip: isPrimaryOnly},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: isPrimaryOnly},
    _flushReshardingStateChange: {skip: isPrimaryOnly},
    _flushRoutingTableCacheUpdates: {skip: isPrimaryOnly},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: isPrimaryOnly},
    _getAuditConfigGeneration: {skip: isNotAUserDataRead},
    _getNextSessionMods: {skip: isPrimaryOnly},
    _getUserCacheGeneration: {skip: isNotAUserDataRead},
    _hashBSONElement: {skip: isNotAUserDataRead},
    _isSelf: {skip: isNotAUserDataRead},
    _killOperations: {skip: isNotAUserDataRead},
    _mergeAuthzCollections: {skip: isPrimaryOnly},
    _migrateClone: {skip: isPrimaryOnly},
    _recvChunkAbort: {skip: isPrimaryOnly},
    _recvChunkCommit: {skip: isPrimaryOnly},
    _recvChunkReleaseCritSec: {skip: isPrimaryOnly},
    _recvChunkStart: {skip: isPrimaryOnly},
    _recvChunkStatus: {skip: isPrimaryOnly},
    _shardsvrAbortReshardCollection: {skip: isPrimaryOnly},
    _shardsvrCleanupReshardCollection: {skip: isPrimaryOnly},
    _shardsvrCloneCatalogData: {skip: isPrimaryOnly},
    _shardsvrCompactStructuredEncryptionData: {skip: isPrimaryOnly},
    _shardsvrCommitReshardCollection: {skip: isPrimaryOnly},
    _shardsvrDropCollection: {skip: isPrimaryOnly},
    _shardsvrCreateCollection: {skip: isPrimaryOnly},
    _shardsvrDropCollectionIfUUIDNotMatching: {skip: isNotAUserDataRead},
    _shardsvrDropCollectionParticipant: {skip: isPrimaryOnly},
    _shardsvrDropIndexes: {skip: isAnInternalCommand},
    _shardsvrCreateCollectionParticipant: {skip: isPrimaryOnly},
    _shardsvrGetStatsForBalancing: {skip: isPrimaryOnly},
    _shardsvrJoinMigrations: {skip: isAnInternalCommand},
    _shardsvrMovePrimary: {skip: isPrimaryOnly},
    _shardsvrMoveRange: {skip: isPrimaryOnly},
    _shardsvrRenameCollection: {skip: isPrimaryOnly},
    _shardsvrRenameCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipantUnblock: {skip: isAnInternalCommand},
    _shardsvrDropDatabase: {skip: isPrimaryOnly},
    _shardsvrDropDatabaseParticipant: {skip: isPrimaryOnly},
    _shardsvrReshardCollection: {skip: isPrimaryOnly},
    _shardsvrReshardingOperationTime: {skip: isPrimaryOnly},
    _shardsvrRefineCollectionShardKey: {skip: isPrimaryOnly},
    _shardsvrSetAllowMigrations: {skip: isPrimaryOnly},
    _shardsvrSetClusterParameter: {skip: isAnInternalCommand},
    _shardsvrSetUserWriteBlockMode: {skip: isPrimaryOnly},
    _shardsvrCollMod: {skip: isPrimaryOnly},
    _shardsvrCollModParticipant: {skip: isAnInternalCommand},
    _shardsvrParticipantBlock: {skip: isAnInternalCommand},
    _transferMods: {skip: isPrimaryOnly},
    _vectorClockPersist: {skip: isPrimaryOnly},
    abortReshardCollection: {skip: isPrimaryOnly},
    abortTransaction: {skip: isPrimaryOnly},
    aggregate: {
        command: {aggregate: collName, pipeline: [{$match: {}}], cursor: {}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    analyze: {skip: isPrimaryOnly},
    appendOplogNote: {skip: isPrimaryOnly},
    applyOps: {skip: isPrimaryOnly},
    authenticate: {skip: isNotAUserDataRead},
    autoSplitVector: {skip: isPrimaryOnly},
    buildInfo: {skip: isNotAUserDataRead},
    captrunc: {skip: isPrimaryOnly},
    checkShardingIndex: {skip: isPrimaryOnly},
    cleanupOrphaned: {skip: isPrimaryOnly},
    cleanupReshardCollection: {skip: isPrimaryOnly},
    clearLog: {skip: isNotAUserDataRead},
    cloneCollectionAsCapped: {skip: isPrimaryOnly},
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {skip: isPrimaryOnly},
    collStats: {
        command: {aggregate: collName, pipeline: [{$collStats: {count: {}}}], cursor: {}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    commitReshardCollection: {skip: isPrimaryOnly},
    commitTransaction: {skip: isPrimaryOnly},
    compact: {skip: isNotAUserDataRead},
    compactStructuredEncryptionData: {skip: isPrimaryOnly},
    configureFailPoint: {skip: isNotAUserDataRead},
    configureCollectionBalancing: {skip: isPrimaryOnly},
    connPoolStats: {skip: isNotAUserDataRead},
    connPoolSync: {skip: isNotAUserDataRead},
    connectionStatus: {skip: isNotAUserDataRead},
    convertToCapped: {skip: isPrimaryOnly},
    coordinateCommitTransaction: {skip: isNotAUserDataRead},
    count: {
        command: {count: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
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
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    dbStats: {
        command: {dbStats: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    delete: {skip: isPrimaryOnly},
    distinct: {
        command: {distinct: collName, key: "a"},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    donorAbortMigration: {skip: isPrimaryOnly},
    donorForgetMigration: {skip: isPrimaryOnly},
    donorStartMigration: {skip: isPrimaryOnly},
    donorWaitForMigrationToCommit: {skip: isPrimaryOnly},
    abortShardSplit: {skip: isPrimaryOnly},
    commitShardSplit: {skip: isPrimaryOnly},
    forgetShardSplit: {skip: isPrimaryOnly},
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
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    features: {skip: isNotAUserDataRead},
    filemd5: {skip: isNotAUserDataRead},
    find: {
        command: {find: collName, filter: {a: 1}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    findAndModify: {skip: isPrimaryOnly},
    flushRouterConfig: {skip: isNotAUserDataRead},
    fsync: {skip: isNotAUserDataRead},
    fsyncUnlock: {skip: isNotAUserDataRead},
    getAuditConfig: {skip: isNotAUserDataRead},
    getClusterParameter: {skip: isNotAUserDataRead},
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
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    getParameter: {skip: isNotAUserDataRead},
    getShardMap: {skip: isNotAUserDataRead},
    getShardVersion: {skip: isPrimaryOnly},
    getnonce: {skip: isNotAUserDataRead},
    godinsert: {skip: isAnInternalCommand},
    grantPrivilegesToRole: {skip: isPrimaryOnly},
    grantRolesToRole: {skip: isPrimaryOnly},
    grantRolesToUser: {skip: isPrimaryOnly},
    hello: {skip: isNotAUserDataRead},
    hostInfo: {skip: isNotAUserDataRead},
    httpClientRequest: {skip: isNotAUserDataRead},
    exportCollection: {skip: isNotAUserDataRead},
    importCollection: {skip: isNotAUserDataRead},
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
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    listCommands: {command: {listCommands: 1}},
    listDatabases: {
        command: {listDatabases: 1},
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    listIndexes: {
        command: {listIndexes: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    lockInfo: {skip: isPrimaryOnly},
    logApplicationMessage: {skip: isNotAUserDataRead},
    logMessage: {skip: isNotAUserDataRead},
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
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    mergeChunks: {skip: isPrimaryOnly},
    moveChunk: {skip: isPrimaryOnly},
    moveRange: {skip: isPrimaryOnly},
    pinHistoryReplicated: {skip: isAnInternalCommand},
    ping: {skip: isNotAUserDataRead},
    planCacheClear: {skip: isNotAUserDataRead},
    planCacheClearFilters: {skip: isNotAUserDataRead},
    planCacheListFilters: {skip: isNotAUserDataRead},
    planCacheSetFilter: {skip: isNotAUserDataRead},
    prepareTransaction: {skip: isPrimaryOnly},
    profile: {skip: isPrimaryOnly},
    reapLogicalSessionCacheNow: {skip: isNotAUserDataRead},
    recipientForgetMigration: {skip: isPrimaryOnly},
    recipientSyncData: {skip: isPrimaryOnly},
    recipientVoteImportedFiles: {skip: isNotAUserDataRead},
    refreshLogicalSessionCacheNow: {skip: isNotAUserDataRead},
    refreshSessions: {skip: isNotAUserDataRead},
    reIndex: {skip: isNotAUserDataRead},
    renameCollection: {skip: isPrimaryOnly},
    repairDatabase: {skip: isNotAUserDataRead},
    repairShardedCollectionChunksHistory: {skip: isPrimaryOnly},
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
    replSetTestEgress: {skip: isNotAUserDataRead},
    replSetUpdatePosition: {skip: isNotAUserDataRead},
    replSetResizeOplog: {skip: isNotAUserDataRead},
    revokePrivilegesFromRole: {skip: isPrimaryOnly},
    revokeRolesFromRole: {skip: isPrimaryOnly},
    revokeRolesFromUser: {skip: isPrimaryOnly},
    rolesInfo: {skip: isPrimaryOnly},
    rotateCertificates: {skip: isNotAUserDataRead},
    saslContinue: {skip: isPrimaryOnly},
    saslStart: {skip: isPrimaryOnly},
    serverStatus: {skip: isNotAUserDataRead},
    setAuditConfig: {skip: isNotAUserDataRead},
    setCommittedSnapshot: {skip: isNotAUserDataRead},
    setDefaultRWConcern: {skip: isPrimaryOnly},
    setIndexCommitQuorum: {skip: isPrimaryOnly},
    setFeatureCompatibilityVersion: {skip: isPrimaryOnly},
    setFreeMonitoring: {skip: isPrimaryOnly},
    setParameter: {skip: isNotAUserDataRead},
    setShardVersion: {skip: isNotAUserDataRead},
    setClusterParameter: {skip: isNotAUserDataRead},
    setUserWriteBlockMode: {skip: isPrimaryOnly},
    shardingState: {skip: isNotAUserDataRead},
    shutdown: {skip: isNotAUserDataRead},
    sleep: {skip: isNotAUserDataRead},
    splitChunk: {skip: isPrimaryOnly},
    splitVector: {skip: isPrimaryOnly},
    stageDebug: {skip: isPrimaryOnly},
    startRecordingTraffic: {skip: isNotAUserDataRead},
    startSession: {skip: isNotAUserDataRead},
    stopRecordingTraffic: {skip: isNotAUserDataRead},
    testDeprecation: {skip: isNotAUserDataRead},
    testDeprecationInVersion2: {skip: isNotAUserDataRead},
    testInternalTransactions: {skip: isNotAUserDataRead},
    testRemoval: {skip: isNotAUserDataRead},
    testReshardCloneCollection: {skip: isNotAUserDataRead},
    testVersions1And2: {skip: isNotAUserDataRead},
    testVersion2: {skip: isNotAUserDataRead},
    top: {skip: isNotAUserDataRead},
    update: {skip: isPrimaryOnly},
    updateRole: {skip: isPrimaryOnly},
    updateUser: {skip: isPrimaryOnly},
    usersInfo: {skip: isPrimaryOnly},
    validate: {skip: isNotAUserDataRead},
    validateDBMetadata: {
        command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    voteCommitImportCollection: {skip: isNotAUserDataRead},
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
