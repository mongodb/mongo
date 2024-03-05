/**
 * This file defines tests for all existing commands and their expected behavior when run against a
 * node that is in RECOVERING state.
 *
 * Tagged as multiversion-incompatible as the list of commands will vary depeding on version.
 * @tags: [multiversion_incompatible]
 */

// This will verify the completeness of our map and run all tests.
import {AllCommandsTest} from "jstests/libs/all_commands_test.js";

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
    _clusterQueryWithoutShardKey: {skip: isAnInternalCommand},
    _clusterWriteWithoutShardKey: {skip: isAnInternalCommand},
    _configsvrAbortReshardCollection: {skip: isPrimaryOnly},
    _configsvrAddShard: {skip: isPrimaryOnly},
    _configsvrAddShardToZone: {skip: isPrimaryOnly},
    _configsvrBalancerCollectionStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStart: {skip: isPrimaryOnly},
    _configsvrBalancerStatus: {skip: isPrimaryOnly},
    _configsvrBalancerStop: {skip: isPrimaryOnly},
    _configsvrCheckClusterMetadataConsistency: {skip: isAnInternalCommand},
    _configsvrCheckMetadataConsistency: {skip: isAnInternalCommand},
    _configsvrCleanupReshardCollection: {skip: isPrimaryOnly},
    _configsvrCollMod: {skip: isAnInternalCommand},
    _configsvrClearJumboFlag: {skip: isPrimaryOnly},
    _configsvrCommitChunksMerge: {skip: isPrimaryOnly},
    _configsvrCommitChunkMigration: {skip: isPrimaryOnly},
    _configsvrCommitChunkSplit: {skip: isPrimaryOnly},
    _configsvrCommitIndex: {skip: isPrimaryOnly},
    _configsvrCommitMergeAllChunksOnShard: {skip: isPrimaryOnly},
    _configsvrCommitMovePrimary: {skip: isPrimaryOnly},
    _configsvrCommitRefineCollectionShardKey: {skip: isPrimaryOnly},
    _configsvrCommitReshardCollection: {skip: isPrimaryOnly},
    _configsvrConfigureCollectionBalancing: {skip: isPrimaryOnly},
    _configsvrCreateDatabase: {skip: isPrimaryOnly},
    _configsvrDropIndexCatalogEntry: {skip: isPrimaryOnly},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isPrimaryOnly},
    _configsvrGetHistoricalPlacement: {skip: isAnInternalCommand},  // TODO SERVER-73029 remove
    _configsvrMoveRange: {skip: isPrimaryOnly},
    _configsvrRefineCollectionShardKey: {skip: isPrimaryOnly},
    _configsvrRemoveChunks: {skip: isPrimaryOnly},
    _configsvrRemoveShard: {skip: isPrimaryOnly},
    _configsvrRemoveShardFromZone: {skip: isPrimaryOnly},
    _configsvrRemoveTags: {skip: isPrimaryOnly},
    _configsvrRepairShardedCollectionChunksHistory: {skip: isPrimaryOnly},
    _configsvrResetPlacementHistory: {skip: isPrimaryOnly},
    _configsvrReshardCollection: {skip: isPrimaryOnly},
    _configsvrRunRestore: {skip: isPrimaryOnly},
    _configsvrSetAllowMigrations: {skip: isPrimaryOnly},
    _configsvrSetClusterParameter: {skip: isPrimaryOnly},
    _configsvrSetUserWriteBlockMode: {skip: isPrimaryOnly},
    _configsvrTransitionFromDedicatedConfigServer: {skip: isPrimaryOnly},
    _configsvrTransitionToDedicatedConfigServer: {skip: isPrimaryOnly},
    _configsvrUpdateZoneKeyRange: {skip: isPrimaryOnly},
    _dropConnectionsToMongot: {skip: isAnInternalCommand},
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
    _mongotConnPoolStats: {skip: isAnInternalCommand},
    _recvChunkAbort: {skip: isPrimaryOnly},
    _recvChunkCommit: {skip: isPrimaryOnly},
    _recvChunkReleaseCritSec: {skip: isPrimaryOnly},
    _recvChunkStart: {skip: isPrimaryOnly},
    _recvChunkStatus: {skip: isPrimaryOnly},
    _refreshQueryAnalyzerConfiguration: {skip: isPrimaryOnly},
    _shardsvrAbortReshardCollection: {skip: isPrimaryOnly},
    _shardsvrChangePrimary: {skip: isAnInternalCommand},
    _shardsvrCleanupStructuredEncryptionData: {skip: isPrimaryOnly},
    _shardsvrCleanupReshardCollection: {skip: isPrimaryOnly},
    _shardsvrCloneCatalogData: {skip: isPrimaryOnly},
    _shardsvrCompactStructuredEncryptionData: {skip: isPrimaryOnly},
    _shardsvrRegisterIndex: {skip: isPrimaryOnly},
    _shardsvrCommitIndexParticipant: {skip: isPrimaryOnly},
    _shardsvrCommitReshardCollection: {skip: isPrimaryOnly},
    _shardsvrConvertToCapped: {skip: isPrimaryOnly},
    _shardsvrCreateGlobalIndex: {skip: isAnInternalCommand},
    _shardsvrDropGlobalIndex: {skip: isAnInternalCommand},
    _shardsvrDropCollection: {skip: isPrimaryOnly},
    _shardsvrCreateCollection: {skip: isPrimaryOnly},
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: isNotAUserDataRead},
    _shardsvrDropCollectionParticipant: {skip: isPrimaryOnly},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: isPrimaryOnly},
    _shardsvrDropIndexes: {skip: isAnInternalCommand},
    _shardsvrCreateCollectionParticipant: {skip: isPrimaryOnly},
    _shardsvrGetStatsForBalancing: {skip: isPrimaryOnly},
    _shardsvrInsertGlobalIndexKey: {skip: isPrimaryOnly},
    _shardsvrDeleteGlobalIndexKey: {skip: isPrimaryOnly},
    _shardsvrWriteGlobalIndexKeys: {skip: isPrimaryOnly},
    _shardsvrJoinMigrations: {skip: isAnInternalCommand},
    _shardsvrMergeAllChunksOnShard: {skip: isPrimaryOnly},
    _shardsvrMovePrimary: {skip: isPrimaryOnly},
    _shardsvrMovePrimaryEnterCriticalSection: {skip: isPrimaryOnly},
    _shardsvrMovePrimaryExitCriticalSection: {skip: isPrimaryOnly},
    _shardsvrMoveRange: {skip: isPrimaryOnly},
    _shardsvrNotifyShardingEvent: {skip: isPrimaryOnly},
    _shardsvrRenameCollection: {skip: isPrimaryOnly},
    _shardsvrRenameCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipantUnblock: {skip: isAnInternalCommand},
    _shardsvrRenameIndexMetadata: {skip: isPrimaryOnly},
    _shardsvrDropDatabase: {skip: isPrimaryOnly},
    _shardsvrDropDatabaseParticipant: {skip: isPrimaryOnly},
    _shardsvrReshardCollection: {skip: isPrimaryOnly},
    _shardsvrReshardingOperationTime: {skip: isPrimaryOnly},
    _shardsvrRefineCollectionShardKey: {skip: isPrimaryOnly},
    _shardsvrSetAllowMigrations: {skip: isPrimaryOnly},
    _shardsvrSetClusterParameter: {skip: isAnInternalCommand},
    _shardsvrSetUserWriteBlockMode: {skip: isPrimaryOnly},
    _shardsvrUnregisterIndex: {skip: isPrimaryOnly},
    _shardsvrValidateShardKeyCandidate: {skip: isPrimaryOnly},
    _shardsvrCoordinateMultiUpdate: {skip: isAnInternalCommand},
    _shardsvrCollMod: {skip: isPrimaryOnly},
    _shardsvrCollModParticipant: {skip: isAnInternalCommand},
    _shardsvrParticipantBlock: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistency: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistencyParticipant: {skip: isAnInternalCommand},
    _shardsvrBeginMigrationBlockingOperation: {skip: isAnInternalCommand},
    _shardsvrEndMigrationBlockingOperation: {skip: isAnInternalCommand},
    streams_startStreamProcessor: {skip: isAnInternalCommand},
    streams_startStreamSample: {skip: isAnInternalCommand},
    streams_stopStreamProcessor: {skip: isAnInternalCommand},
    streams_listStreamProcessors: {skip: isAnInternalCommand},
    streams_getMoreStreamSample: {skip: isAnInternalCommand},
    streams_getStats: {skip: isAnInternalCommand},
    streams_testOnlyInsert: {skip: isAnInternalCommand},
    streams_getMetrics: {skip: isAnInternalCommand},
    _transferMods: {skip: isPrimaryOnly},
    _vectorClockPersist: {skip: isPrimaryOnly},
    abortMoveCollection: {skip: isPrimaryOnly},
    abortReshardCollection: {skip: isPrimaryOnly},
    abortTransaction: {skip: isPrimaryOnly},
    abortUnshardCollection: {skip: isPrimaryOnly},
    aggregate: {
        command: {aggregate: collName, pipeline: [{$match: {}}], cursor: {}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    analyze: {skip: isPrimaryOnly},
    analyzeShardKey: {
        command: {analyzeShardKey: fullNs, key: {skey: 1}},
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    appendOplogNote: {skip: isPrimaryOnly},
    applyOps: {skip: isPrimaryOnly},
    authenticate: {skip: isNotAUserDataRead},
    autoCompact: {skip: isNotAUserDataRead},
    autoSplitVector: {skip: isPrimaryOnly},
    buildInfo: {skip: isNotAUserDataRead},
    bulkWrite: {skip: isPrimaryOnly},
    captrunc: {skip: isPrimaryOnly},
    changePrimary: {skip: isPrimaryOnly},
    checkMetadataConsistency: {skip: isPrimaryOnly},
    checkShardingIndex: {skip: isPrimaryOnly},
    cleanupOrphaned: {skip: isPrimaryOnly},
    cleanupReshardCollection: {skip: isPrimaryOnly},
    cleanupStructuredEncryptionData: {skip: isPrimaryOnly},
    clearLog: {skip: isNotAUserDataRead},
    cloneCollectionAsCapped: {skip: isPrimaryOnly},
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterBulkWrite: {skip: "already tested by 'bulkWrite' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterCount: {skip: "already tested by 'count' tests on mongos"},
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
    configureQueryAnalyzer: {skip: isPrimaryOnly},
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
    createSearchIndexes: {skip: isNotAUserDataRead},
    createUnsplittableCollection: {skip: isPrimaryOnly},
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
    drop: {skip: isPrimaryOnly},
    dropAllRolesFromDatabase: {skip: isPrimaryOnly},
    dropAllUsersFromDatabase: {skip: isPrimaryOnly},
    dropConnections: {skip: isNotAUserDataRead},
    dropDatabase: {skip: isPrimaryOnly},
    dropIndexes: {skip: isPrimaryOnly},
    dropRole: {skip: isPrimaryOnly},
    dropSearchIndex: {skip: isNotAUserDataRead},
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
    getChangeStreamState: {skip: isNotAUserDataRead},
    getClusterParameter: {skip: isNotAUserDataRead},
    getCmdLineOpts: {skip: isNotAUserDataRead},
    getDatabaseVersion: {skip: isNotAUserDataRead},
    getDefaultRWConcern: {skip: isNotAUserDataRead},
    getDiagnosticData: {skip: isNotAUserDataRead},
    getLog: {skip: isNotAUserDataRead},
    getMore: {
        command: {getMore: NumberLong(123), collection: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    getQueryableEncryptionCountInfo: {skip: isPrimaryOnly},
    getParameter: {skip: isNotAUserDataRead},
    getShardMap: {skip: isNotAUserDataRead},
    getShardVersion: {skip: isPrimaryOnly},
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
    listDatabasesForAllTenants: {
        command: {listDatabasesForAllTenants: 1},
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    listIndexes: {
        command: {listIndexes: collName},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary
    },
    listSearchIndexes: {skip: isNotAUserDataRead},
    lockInfo: {skip: isAnInternalCommand},
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
    mergeAllChunksOnShard: {skip: "primary only"},
    mergeChunks: {skip: isPrimaryOnly},
    moveChunk: {skip: isPrimaryOnly},
    moveRange: {skip: isPrimaryOnly},
    oidcListKeys: {skip: isNotAUserDataRead},
    oidcRefreshKeys: {skip: isNotAUserDataRead},
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
    resetPlacementHistory: {skip: isPrimaryOnly},
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
    setProfilingFilterGlobally: {skip: isNotAUserDataRead},
    setParameter: {skip: isNotAUserDataRead},
    setShardVersion: {skip: isNotAUserDataRead},
    setChangeStreamState: {skip: isNotAUserDataRead},
    setClusterParameter: {skip: isNotAUserDataRead},
    setQuerySettings: {skip: isPrimaryOnly},
    removeQuerySettings: {skip: isPrimaryOnly},
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
    sysprofile: {skip: isAnInternalCommand},
    testDeprecation: {skip: isNotAUserDataRead},
    testDeprecationInVersion2: {skip: isNotAUserDataRead},
    testInternalTransactions: {skip: isNotAUserDataRead},
    testRemoval: {skip: isNotAUserDataRead},
    testReshardCloneCollection: {skip: isNotAUserDataRead},
    testVersions1And2: {skip: isNotAUserDataRead},
    testVersion2: {skip: isNotAUserDataRead},
    timeseriesCatalogBucketParamsChanged: {skip: isAnInternalCommand},
    top: {skip: isNotAUserDataRead},
    transitionToShardedCluster: {skip: isNotAUserDataRead},
    update: {skip: isPrimaryOnly},
    updateRole: {skip: isPrimaryOnly},
    updateSearchIndex: {skip: isNotAUserDataRead},
    updateUser: {skip: isPrimaryOnly},
    usersInfo: {skip: isPrimaryOnly},
    validate: {skip: isNotAUserDataRead},
    validateDBMetadata: {
        command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NotPrimaryOrSecondary,
    },
    voteAbortIndexBuild: {skip: isNotAUserDataRead},
    voteCommitImportCollection: {skip: isNotAUserDataRead},
    voteCommitIndexBuild: {skip: isNotAUserDataRead},
    waitForFailPoint: {skip: isNotAUserDataRead},
    getShardingReady: {skip: isNotAUserDataRead},
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
