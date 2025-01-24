/**
 * This file defines tests for all existing commands and their expected behavior when run directly
 * against a node that is part of a multi-shard cluster.
 *
 * @tags: [
 *   # Tagged as multiversion-incompatible as the list of commands will vary depending on version.
 *   multiversion_incompatible,
 *   # Cannot compact when using the in-memory storage engine.
 *   requires_persistence,
 *   featureFlagFailOnDirectShardOperations,
 *   requires_fcv_73
 * ]
 */

// This will verify the completeness of our map and run all tests.
import {AllCommandsTest} from "jstests/libs/all_commands_test.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "coll";
const fullNs = dbName + "." + collName;

// Pre-written reasons for skipping a test.
const isAnInternalCommand = "internal command";
const isDeprecated = "deprecated command";
const requiresParallelShell = "requires parallel shell";
const requiresMongoS = "command only allowed via mongoS";

const allCommands = {
    _addShard: {skip: isAnInternalCommand},
    _clusterQueryWithoutShardKey: {skip: isAnInternalCommand},
    _clusterWriteWithoutShardKey: {skip: isAnInternalCommand},
    _configsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _configsvrAddShard: {skip: isAnInternalCommand},
    _configsvrAddShardCoordinator: {skip: isAnInternalCommand},
    _configsvrAddShardToZone: {skip: isAnInternalCommand},
    _configsvrBalancerCollectionStatus: {skip: isAnInternalCommand},
    _configsvrBalancerStart: {skip: isAnInternalCommand},
    _configsvrBalancerStatus: {skip: isAnInternalCommand},
    _configsvrBalancerStop: {skip: isAnInternalCommand},
    _configsvrCheckClusterMetadataConsistency: {skip: isAnInternalCommand},
    _configsvrCheckMetadataConsistency: {skip: isAnInternalCommand},
    _configsvrCleanupReshardCollection: {skip: isAnInternalCommand},
    _configsvrCollMod: {skip: isAnInternalCommand},
    _configsvrClearJumboFlag: {skip: isAnInternalCommand},
    _configsvrCommitChunksMerge: {skip: isAnInternalCommand},
    _configsvrCommitChunkMigration: {skip: isAnInternalCommand},
    _configsvrCommitChunkSplit: {skip: isAnInternalCommand},
    _configsvrCommitIndex: {skip: isAnInternalCommand},
    _configsvrCommitMergeAllChunksOnShard: {skip: isAnInternalCommand},
    _configsvrCommitMovePrimary: {skip: isAnInternalCommand},
    _configsvrCommitRefineCollectionShardKey: {skip: isAnInternalCommand},
    _configsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _configsvrConfigureCollectionBalancing: {skip: isAnInternalCommand},
    _configsvrCreateDatabase: {skip: isAnInternalCommand},
    _configsvrDropIndexCatalogEntry: {skip: isAnInternalCommand},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isAnInternalCommand},
    _configsvrGetHistoricalPlacement: {skip: isAnInternalCommand},  // TODO SERVER-73029 remove
    _configsvrMoveRange: {skip: isAnInternalCommand},
    _configsvrRemoveChunks: {skip: isAnInternalCommand},
    _configsvrRemoveShard: {skip: isAnInternalCommand},
    _configsvrRemoveShardCommit: {skip: isAnInternalCommand},
    _configsvrRemoveShardFromZone: {skip: isAnInternalCommand},
    _configsvrRemoveTags: {skip: isAnInternalCommand},
    _configsvrRepairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    _configsvrResetPlacementHistory: {skip: isAnInternalCommand},
    _configsvrReshardCollection: {skip: isAnInternalCommand},
    _configsvrRunRestore: {skip: isAnInternalCommand},
    _configsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _configsvrSetClusterParameter: {skip: isAnInternalCommand},
    _configsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _configsvrTransitionFromDedicatedConfigServer: {skip: isAnInternalCommand},
    _configsvrTransitionToDedicatedConfigServer: {skip: isAnInternalCommand},
    _configsvrUpdateZoneKeyRange: {skip: isAnInternalCommand},
    _dropConnectionsToMongot: {skip: isAnInternalCommand},
    _flushDatabaseCacheUpdates: {skip: isAnInternalCommand},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: isAnInternalCommand},
    _flushReshardingStateChange: {skip: isAnInternalCommand},
    _flushRoutingTableCacheUpdates: {skip: isAnInternalCommand},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: isAnInternalCommand},
    _getNextSessionMods: {skip: isAnInternalCommand},
    _getUserCacheGeneration: {skip: isAnInternalCommand},
    _hashBSONElement: {skip: isAnInternalCommand},
    _isSelf: {skip: isAnInternalCommand},
    _killOperations: {skip: isAnInternalCommand},
    _mergeAuthzCollections: {skip: isAnInternalCommand},
    _migrateClone: {skip: isAnInternalCommand},
    _mongotConnPoolStats: {skip: isAnInternalCommand},
    _recvChunkAbort: {skip: isAnInternalCommand},
    _recvChunkCommit: {skip: isAnInternalCommand},
    _recvChunkReleaseCritSec: {skip: isAnInternalCommand},
    _recvChunkStart: {skip: isAnInternalCommand},
    _recvChunkStatus: {skip: isAnInternalCommand},
    _refreshQueryAnalyzerConfiguration: {skip: isAnInternalCommand},
    _shardsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _shardsvrBeginMigrationBlockingOperation: {skip: isAnInternalCommand},
    _shardsvrChangePrimary: {skip: isAnInternalCommand},
    _shardsvrCleanupStructuredEncryptionData: {skip: isAnInternalCommand},
    _shardsvrCleanupReshardCollection: {skip: isAnInternalCommand},
    _shardsvrCloneCatalogData: {skip: isAnInternalCommand},
    _shardsvrCompactStructuredEncryptionData: {skip: isAnInternalCommand},
    _shardsvrConvertToCapped: {skip: isAnInternalCommand},
    _shardsvrRegisterIndex: {skip: isAnInternalCommand},
    _shardsvrCommitIndexParticipant: {skip: isAnInternalCommand},
    _shardsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _shardsvrDropCollection: {skip: isAnInternalCommand},
    _shardsvrCreateCollection: {skip: isAnInternalCommand},
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: isAnInternalCommand},
    _shardsvrDropCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: isAnInternalCommand},
    _shardsvrDropIndexes: {skip: isAnInternalCommand},
    _shardsvrCreateCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrCoordinateMultiUpdate: {skip: isAnInternalCommand},
    _shardsvrEndMigrationBlockingOperation: {skip: isAnInternalCommand},
    _shardsvrGetStatsForBalancing: {skip: isAnInternalCommand},
    _shardsvrJoinDDLCoordinators: {skip: isAnInternalCommand},
    _shardsvrJoinMigrations: {skip: isAnInternalCommand},
    _shardsvrMergeAllChunksOnShard: {skip: isAnInternalCommand},
    _shardsvrMovePrimary: {skip: isAnInternalCommand},
    _shardsvrMovePrimaryEnterCriticalSection: {skip: isAnInternalCommand},
    _shardsvrMovePrimaryExitCriticalSection: {skip: isAnInternalCommand},
    _shardsvrMoveRange: {skip: isAnInternalCommand},
    _shardsvrNotifyShardingEvent: {skip: isAnInternalCommand},
    _shardsvrRenameCollection: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipantUnblock: {skip: isAnInternalCommand},
    _shardsvrRenameIndexMetadata: {skip: isAnInternalCommand},
    _shardsvrRunSearchIndexCommand: {skip: isAnInternalCommand},
    _shardsvrDropDatabase: {skip: isAnInternalCommand},
    _shardsvrDropDatabaseParticipant: {skip: isAnInternalCommand},
    _shardsvrReshardCollection: {skip: isAnInternalCommand},
    _shardsvrReshardingOperationTime: {skip: isAnInternalCommand},
    _shardsvrReshardRecipientClone: {skip: isAnInternalCommand},
    _shardsvrRefineCollectionShardKey: {skip: isAnInternalCommand},
    _shardsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _shardsvrSetClusterParameter: {skip: isAnInternalCommand},
    _shardsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _shardsvrUnregisterIndex: {skip: isAnInternalCommand},
    _shardsvrValidateShardKeyCandidate: {skip: isAnInternalCommand},
    _shardsvrCollMod: {skip: isAnInternalCommand},
    _shardsvrCollModParticipant: {skip: isAnInternalCommand},
    _shardsvrConvertToCappedParticipant: {skip: isAnInternalCommand},
    _shardsvrParticipantBlock: {skip: isAnInternalCommand},
    _shardsvrUntrackUnsplittableCollection: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistency: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistencyParticipant: {skip: isAnInternalCommand},
    streams_startStreamProcessor: {skip: isAnInternalCommand},
    streams_startStreamSample: {skip: isAnInternalCommand},
    streams_stopStreamProcessor: {skip: isAnInternalCommand},
    streams_listStreamProcessors: {skip: isAnInternalCommand},
    streams_getMoreStreamSample: {skip: isAnInternalCommand},
    streams_getStats: {skip: isAnInternalCommand},
    streams_testOnlyInsert: {skip: isAnInternalCommand},
    streams_getMetrics: {skip: isAnInternalCommand},
    streams_updateFeatureFlags: {skip: isAnInternalCommand},
    streams_testOnlyGetFeatureFlags: {skip: isAnInternalCommand},
    streams_writeCheckpoint: {skip: isAnInternalCommand},
    streams_sendEvent: {skip: isAnInternalCommand},
    _transferMods: {skip: isAnInternalCommand},
    abortMoveCollection: {
        // Skipping command because it requires testing through a parallel shell.
        skip: requiresParallelShell,
    },
    abortReshardCollection: {
        // Skipping command because it requires testing through a parallel shell.
        skip: requiresParallelShell,
    },
    abortTransaction: {skip: "Requires changes to permissions or number of shards"},
    abortUnshardCollection: {
        // Skipping command because it requires testing through a parallel shell.
        skip: requiresParallelShell,
    },
    addShard: {
        skip: requiresMongoS,
    },
    addShardToZone: {
        skip: requiresMongoS,
    },
    aggregate: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {
            aggregate: collName,
            pipeline: [
                {$match: {}},
                {$skip: 1},
                {$count: "count"},
                {$project: {_id: 1}},
                {$sort: {_id: 1}},
                {$limit: 1},
                {$set: {x: "1"}},
            ],
            cursor: {}
        },
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    analyze: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {analyze: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    analyzeShardKey: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                mongoS.getDB('admin').runCommand({shardCollection: fullNs, key: {_id: 1}}));
            for (let i = 0; i < 1000; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {analyzeShardKey: fullNs, key: {_id: 1}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    appendOplogNote: {
        command: {appendOplogNote: 1, data: {a: 1}},
        shouldFail: false,
        isAdminCommand: true,
    },
    applyOps: {
        command: {applyOps: [{"op": "c", "ns": dbName + ".$cmd", "o": {"create": collName}}]},
        isAdminCommand: true,
        shouldFail: true,
    },
    authenticate: {skip: "tested in test setup and direct_shard_connection_auth.js"},
    autoCompact: {
        checkFeatureFlag: "AutoCompact",
        command: {autoCompact: false},
        isAdminCommand: true,
        shouldFail: false,
    },
    autoSplitVector: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                mongoS.getDB('admin').runCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {
            autoSplitVector: collName,
            keyPattern: {a: 1},
            min: {a: MinKey},
            max: {a: MaxKey},
            maxChunkSizeBytes: 1024 * 1024
        },
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    balancerCollectionStatus: {skip: requiresMongoS},
    balancerStart: {skip: requiresMongoS},
    balancerStatus: {skip: requiresMongoS},
    balancerStop: {skip: requiresMongoS},
    buildInfo: {
        command: {buildInfo: 1},
        shouldFail: false,
        isAdminCommand: true,
    },
    bulkWrite: {
        // TODO SERVER-67711: Remove check when this feature flag is removed.
        checkFeatureFlag: "BulkWriteCommand",
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            bulkWrite: 1,
            ops: [
                {insert: 0, document: {skey: "MongoDB"}},
                {insert: 0, document: {skey: "MongoDB"}}
            ],
            nsInfo: [{ns: fullNs}]
        },
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
        isAdminCommand: true,
    },
    changePrimary: {skip: requiresMongoS},
    checkMetadataConsistency: {skip: requiresMongoS},
    checkShardingIndex: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            const coll = mongoS.getCollection(fullNs);
            coll.createIndex({x: 1, y: 1});
        },
        command: {checkShardingIndex: fullNs, keyPattern: {x: 1, y: 1}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        }
    },
    cleanupOrphaned: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {cleanupOrphaned: fullNs},
        isAdminCommand: true,
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    cleanupReshardCollection: {
        // Skipping command because it requires additional setup through a failed resharding
        // operation.
        skip: "requires additional setup through a failed resharding operation",
    },
    cleanupStructuredEncryptionData: {skip: "requires additional encrypted collection setup"},
    clearJumboFlag: {skip: requiresMongoS},
    clearLog: {
        command: {clearLog: 'global'},
        shouldFail: false,
        isAdminCommand: true,
    },
    cloneCollectionAsCapped: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {
            cloneCollectionAsCapped: collName,
            toCollection: collName + "2",
            size: 10 * 1024 * 1024
        },
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName + "2"}));
        },
    },
    clusterAbortTransaction: {skip: requiresMongoS},
    clusterAggregate: {skip: requiresMongoS},
    clusterBulkWrite: {skip: requiresMongoS},
    clusterCommitTransaction: {skip: requiresMongoS},
    clusterCount: {skip: requiresMongoS},
    clusterDelete: {skip: requiresMongoS},
    clusterFind: {skip: requiresMongoS},
    clusterGetMore: {skip: requiresMongoS},
    clusterInsert: {skip: requiresMongoS},
    clusterUpdate: {skip: requiresMongoS},
    collMod: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {collMod: collName, validator: {}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    collStats: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {aggregate: collName, pipeline: [{$collStats: {count: {}}}], cursor: {}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    commitReshardCollection: {
        skip: requiresParallelShell,
    },
    commitTransaction: {skip: "requires modifications to users of number of shards"},
    compact: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {compact: collName, force: true},
        shouldFail: false,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    compactStructuredEncryptionData: {skip: "requires additional encrypted collection setup"},
    configureFailPoint: {skip: isAnInternalCommand},
    configureCollectionBalancing: {skip: requiresMongoS},
    configureQueryAnalyzer: {skip: requiresMongoS},
    connPoolStats: {
        isAdminCommand: true,
        command: {connPoolStats: 1},
        shouldFail: false,
    },
    connPoolSync: {isAdminCommand: true, command: {connPoolSync: 1}, shouldFail: false},
    connectionStatus: {isAdminCommand: true, command: {connectionStatus: 1}, shouldFail: false},
    convertToCapped: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {convertToCapped: collName, size: 10 * 1024 * 1024},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    coordinateCommitTransaction: {skip: isAnInternalCommand},
    count: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {count: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    cpuload: {skip: isAnInternalCommand},
    create: {
        command: {create: collName},
        shouldFail: true,
    },
    createIndexes: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(mongoS.getCollection(fullNs).insert({x: 1}));
        },
        command: {createIndexes: collName, indexes: [{key: {x: 1}, name: "foo"}]},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    createRole: {
        command: {createRole: "foo", privileges: [], roles: []},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    createSearchIndexes: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    createUnsplittableCollection: {skip: isAnInternalCommand},
    createUser: {
        command: {createUser: "foo", pwd: "bar", roles: []},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    currentOp: {
        command: {currentOp: 1},
        shouldFail: false,
        isAdminCommand: true,
    },
    dataSize: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {dataSize: fullNs},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    dbCheck: {command: {dbCheck: 1}, shouldFail: true},
    dbHash: {command: {dbHash: 1}, shouldFail: false},
    dbStats: {
        command: {dbStats: 1},
        shouldFail: true,
    },
    delete: {
        setUp: function(mongoS) {
            assert.commandWorked(
                mongoS.getCollection(fullNs).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {delete: collName, deletes: [{q: {x: 1}, limit: 1}]},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    distinct: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {distinct: collName, key: "a"},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    donorAbortMigration: {skip: isAnInternalCommand},
    donorForgetMigration: {skip: isAnInternalCommand},
    donorStartMigration: {skip: isAnInternalCommand},
    abortShardSplit: {skip: isDeprecated},
    commitShardSplit: {skip: isDeprecated},
    forgetShardSplit: {skip: isDeprecated},
    drop: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {drop: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        }
    },
    dropAllRolesFromDatabase: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
        },
        command: {dropAllRolesFromDatabase: 1},
        shouldFail: false,
    },
    dropAllUsersFromDatabase: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {dropAllUsersFromDatabase: 1},
        shouldFail: false,
    },
    dropConnections: {
        skip: "requires additional setup to reconfig and add/remove nodes",
    },
    dropDatabase: {
        command: {dropDatabase: 1},
        shouldFail: true,
    },
    dropIndexes: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(mongoS.getCollection(fullNs).insert({x: 1}));
            assert.commandWorked(mongoS.getDB(dbName).runCommand(
                {createIndexes: collName, indexes: [{key: {x: 1}, name: "foo"}]}));
        },
        command: {dropIndexes: collName, index: {x: 1}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    dropRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
        },
        command: {dropRole: "foo"},
        shouldFail: false,
    },
    dropSearchIndex: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    dropUser: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {dropUser: "foo"},
        shouldFail: false
    },
    echo: {command: {echo: 1}, shouldFail: false},
    enableSharding: {
        skip: requiresMongoS,
    },
    endSessions: {skip: "tested in startSession"},
    explain: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {explain: {count: collName}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    features: {command: {features: 1}, shouldFail: false},
    filemd5: {
        setUp: function(mongoS) {
            const f = mongoS.getCollection(dbName + ".fs.chunks");
            assert.commandWorked(f.createIndex({files_id: 1, n: 1}));
        },
        command: {filemd5: 1, root: "fs"},
        shouldFail: true,
    },
    find: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(mongoS.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {find: collName, filter: {a: 1}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    findAndModify: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getCollection(fullNs).insert({x: 1}));
        },
        command: {findAndModify: collName, query: {x: 1}, update: {$set: {x: 2}}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        }
    },
    flushRouterConfig: {isAdminCommand: true, command: {flushRouterConfig: 1}, shouldFail: false},
    fsync: {
        command: {fsync: 1},
        isAdminCommand: true,
        shouldFail: false,
    },
    fsyncUnlock: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(
                withDirectConnections.getDB('admin').runCommand({fsync: 1, lock: 1}));
        },
        command: {fsyncUnlock: 1},
        shouldFail: false,
        isAdminCommand: true,
    },
    getAuditConfig: {
        isAdminCommand: true,
        command: {getAuditConfig: 1},
        shouldFail: false,
    },
    getChangeStreamState: {skip: "Only supported on serverless"},
    getClusterParameter: {
        isAdminCommand: true,
        command: {getClusterParameter: "changeStreamOptions"},
        shouldFail: false,
    },
    getCmdLineOpts: {
        isAdminCommand: true,
        command: {getCmdLineOpts: 1},
        shouldFail: false,
    },
    getDatabaseVersion: {
        isAdminCommand: true,
        command: {getDatabaseVersion: dbName},
        shouldFail: true,
    },
    getDefaultRWConcern: {skip: requiresMongoS},
    getDiagnosticData: {
        isAdminCommand: true,
        command: {getDiagnosticData: 1},
        shouldFail: false,
    },
    getLog: {
        isAdminCommand: true,
        command: {getLog: "global"},
        shouldFail: false,
    },
    getMore: {
        skip: "requires instantiating a cursor",
    },
    getParameter:
        {isAdminCommand: true, command: {getParameter: 1, logLevel: 1}, shouldFail: false},
    getQueryableEncryptionCountInfo: {skip: isAnInternalCommand},
    getShardMap: {
        isAdminCommand: true,
        command: {getShardMap: 1},
        shouldFail: false,
    },
    getShardVersion: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {getShardVersion: fullNs},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
        isAdminCommand: true,
    },
    godinsert: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {godinsert: collName, obj: {_id: 0, a: 0}},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    grantPrivilegesToRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(
                withDirectConnections.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            grantPrivilegesToRole: "foo",
            privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
        },
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({drop: collName}));
        }
    },
    grantRolesToRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "bar", privileges: [], roles: []}));
        },
        command: {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "bar"}));
        }
    },
    grantRolesToUser: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    hello: {
        isAdminCommand: true,
        command: {hello: 1},
        shouldFail: false,
    },
    hostInfo: {isAdminCommand: true, command: {hostInfo: 1}, shouldFail: false},
    httpClientRequest: {skip: isAnInternalCommand},
    exportCollection: {skip: isAnInternalCommand},
    importCollection: {skip: isAnInternalCommand},
    insert: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {insert: collName, documents: [{_id: ObjectId()}]},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: isAnInternalCommand},
    invalidateUserCache: {
        isAdminCommand: 1,
        command: {invalidateUserCache: 1},
        shouldFail: false,
    },
    isdbgrid: {skip: requiresMongoS},
    isMaster: {
        isAdminCommand: 1,
        command: {isMaster: 1},
        shouldFail: false,
    },
    killAllSessions: {
        command: {killAllSessions: []},
        shouldFail: false,
    },
    killAllSessionsByPattern: {
        command: {killAllSessionsByPattern: []},
        shouldFail: false,
    },
    killCursors: {skip: "requires instantiating a cursor"},
    killOp: {skip: requiresParallelShell},
    killSessions: {skip: "Requires changes to roles or number of shards"},
    listCollections: {
        command: {listCollections: 1},
        shouldFail: true,
    },
    listCommands: {command: {listCommands: 1}, shouldFail: false},
    listDatabases: {
        // List databases will fail if it is run without nameOnly and there are databases other than
        // config/local/admin present. It will not fail with nameOnly set to true.
        fullScenario: function(mongoS, withDirectConnections, withoutDirectConnections) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));

            assert.commandWorked(
                withoutDirectConnections.adminCommand({listDatabases: 1, nameOnly: 1}));
            assert.commandFailedWithCode(withoutDirectConnections.adminCommand({listDatabases: 1}),
                                         ErrorCodes.Unauthorized);

            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        }
    },
    listDatabasesForAllTenants: {
        skip: isAnInternalCommand,
    },
    listIndexes: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {listIndexes: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    listSearchIndexes: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    listShards: {skip: requiresMongoS},
    lockInfo: {
        isAdminCommand: true,
        command: {lockInfo: 1},
        shouldFail: false,
    },
    logApplicationMessage: {
        isAdminCommand: true,
        command: {logApplicationMessage: "hello"},
        shouldFail: false,
    },
    logMessage: {
        skip: isAnInternalCommand,
    },
    logRotate: {
        isAdminCommand: true,
        command: {logRotate: 1},
        shouldFail: false,
    },
    logout: {
        skip: "requires additional authentication setup",
    },
    makeSnapshot: {
        isAdminCommand: true,
        command: {makeSnapshot: 1},
        shouldFail: false,
    },
    mapReduce: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            mapReduce: collName,
            map: function() {},
            reduce: function(key, vals) {},
            out: {inline: 1}
        },
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    mergeAllChunksOnShard: {skip: requiresMongoS},
    mergeChunks: {skip: requiresMongoS},
    moveChunk: {skip: requiresMongoS},
    moveCollection: {skip: requiresMongoS},
    movePrimary: {skip: requiresMongoS},
    moveRange: {skip: requiresMongoS},
    multicast: {skip: requiresMongoS},
    netstat: {
        skip: isAnInternalCommand,
    },
    oidcListKeys: {
        // Skipping this command as it requires OIDC/OpenSSL setup.
        skip: "requires additional OIDC/OpenSSL setup",
    },
    oidcRefreshKeys: {
        // Skipping this command as it requires OIDC/OpenSSL setup.
        skip: "requires additional OIDC/OpenSSL setup",
    },
    pinHistoryReplicated: {
        skip: isAnInternalCommand,
    },
    ping: {isAdminCommand: true, command: {ping: 1}, shouldFail: false},
    planCacheClear: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheClear: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheClearFilters: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheClearFilters: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheListFilters: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheListFilters: collName},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheSetFilter: {
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        },
    },
    prepareTransaction: {skip: isAnInternalCommand},
    profile: {
        isAdminCommand: true,
        command: {profile: 2},
        shouldFail: false,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB('admin').runCommand({profile: 0}));
        },
    },
    reapLogicalSessionCacheNow: {
        isAdminCommand: true,
        command: {reapLogicalSessionCacheNow: 1},
        shouldFail: false,
    },
    recipientForgetMigration: {skip: isAnInternalCommand},
    recipientSyncData: {skip: isAnInternalCommand},
    recipientVoteImportedFiles: {skip: isAnInternalCommand},
    refineCollectionShardKey: {skip: requiresMongoS},
    refreshLogicalSessionCacheNow: {
        command: {refreshLogicalSessionCacheNow: 1},
        shouldFail: false,
        isAdminCommand: true,
    },
    refreshSessions: {skip: "requires changes to roles or number of shards"},
    reIndex: {
        skip: isDeprecated,
    },
    removeShard: {skip: requiresMongoS},
    removeShardFromZone: {skip: requiresMongoS},
    renameCollection: {
        isAdminCommand: true,
        setUp: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({create: collName}));
        },
        command: {renameCollection: fullNs, to: fullNs + "2"},
        shouldFail: true,
        teardown: function(mongoS) {
            assert.commandWorked(mongoS.getDB(dbName).runCommand({drop: collName}));
        }
    },
    repairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    replSetAbortPrimaryCatchUp: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetFreeze: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetGetConfig: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetGetRBID: {skip: isAnInternalCommand},
    replSetGetStatus: {
        isReplSetOnly: true,
        isAdminCommand: true,
        command: {replSetGetStatus: 1},
        shouldFail: false
    },
    replSetHeartbeat: {skip: isAnInternalCommand},
    replSetInitiate: {skip: "must be run before shard is added to the cluster"},
    replSetMaintenance: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetReconfig: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetRequestVotes: {skip: isAnInternalCommand},
    replSetStepDown: {
        skip: "tested in direct_shard_connection_auth_rs_commands.js",
    },
    replSetStepUp: {
        skip: "tested in direct_shard_connection_auth_rs_commands.js",
    },
    replSetSyncFrom: {skip: "tested in direct_shard_connection_auth_rs_commands.js"},
    replSetTest: {skip: isAnInternalCommand},
    replSetTestEgress: {skip: isAnInternalCommand},
    replSetUpdatePosition: {skip: isAnInternalCommand},
    replSetResizeOplog: {
        isReplSetOnly: true,
        isAdminCommand: true,
        command: {replSetResizeOplog: 1, minRetentionHours: 1},
        shouldFail: false,
    },
    resetPlacementHistory: {skip: requiresMongoS},
    reshardCollection: {skip: requiresMongoS},
    revokePrivilegesFromRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(
                withDirectConnections.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({
                createRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                roles: [],
            }));
        },
        command: {
            revokePrivilegesFromRole: "foo",
            privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
        },
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({drop: collName}));
        }
    },
    revokeRolesFromRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "bar", privileges: [], roles: []}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({
                createRole: "foo",
                privileges: [],
                roles: [{role: "bar", db: dbName}],
            }));
        },
        command: {revokeRolesFromRole: "foo", roles: [{role: "foo", db: dbName}]},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "bar"}));
        }
    },
    revokeRolesFromUser: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({
                createUser: "foo",
                pwd: "bar",
                roles: [{role: "foo", db: dbName}],
            }));
        },
        command: {revokeRolesFromUser: "foo", roles: [{role: "foo", db: dbName}]},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    rolesInfo: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
        },
        command: {rolesInfo: 1},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    rotateCertificates: {skip: "requires additional authentication setup"},
    rotateFTDC: {isAdminCommand: true, command: {rotateFTDC: 1}, shouldFail: false},
    saslContinue: {skip: "requires additional authentication setup"},
    saslStart: {skip: "requires additional authentication setup"},
    serverStatus: {
        isAdminCommand: true,
        command: {serverStatus: 1},
        shouldFail: false,
    },
    setAuditConfig: {skip: "requires additional audit/authentication setup"},
    setAllowMigrations: {skip: requiresMongoS},
    setCommittedSnapshot: {skip: isAnInternalCommand},
    setDefaultRWConcern: {skip: requiresMongoS},
    setIndexCommitQuorum: {skip: requiresParallelShell},
    setFeatureCompatibilityVersion: {skip: requiresMongoS},
    setProfilingFilterGlobally: {skip: "requires special startup parameter"},
    setParameter: {
        command: {setParameter: 1, quiet: 1},
        isAdminCommand: true,
        shouldFail: false,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand({setParameter: 1, quiet: 0}));
        }
    },
    setChangeStreamState: {skip: "requires serverless"},
    setClusterParameter: {skip: requiresMongoS},
    setQuerySettings: {skip: requiresMongoS},
    removeQuerySettings: {skip: requiresMongoS},
    setUserWriteBlockMode: {skip: requiresMongoS},
    shardCollection: {skip: requiresMongoS},
    shardingState: {isAdminCommand: true, command: {shardingState: 1}, shouldFail: false},
    shutdown: {skip: "requires changes to shards"},
    sleep: {skip: isAnInternalCommand},
    split: {skip: requiresMongoS},
    splitChunk: {skip: isAnInternalCommand},
    splitVector: {skip: isAnInternalCommand},
    stageDebug: {skip: isAnInternalCommand},
    startRecordingTraffic: {
        // Skipping command because it requires an actual file path for recording traffic to.
        skip: "requires an actual file path to record traffic to",
    },
    startSession: {
        fullScenario: function(mongoS, withDirectConnections, withoutDirectConnections) {
            const res = withoutDirectConnections.adminCommand({startSession: 1});
            assert.commandWorked(res);
            assert.commandWorked(withoutDirectConnections.adminCommand({endSessions: [res.id]}));
        }
    },
    stopRecordingTraffic: {
        // Skipping command because it requires an actual file path for recording traffic to.
        skip: "requires an actual file path to record traffic to",
    },
    sysprofile: {skip: isAnInternalCommand},
    testDeprecation: {skip: isAnInternalCommand},
    testDeprecationInVersion2: {skip: isAnInternalCommand},
    testInternalTransactions: {skip: isAnInternalCommand},
    testRemoval: {skip: isAnInternalCommand},
    testReshardCloneCollection: {skip: isAnInternalCommand},
    testVersions1And2: {skip: isAnInternalCommand},
    testVersion2: {skip: isAnInternalCommand},
    timeseriesCatalogBucketParamsChanged: {skip: isAnInternalCommand},
    top: {
        command: {top: 1},
        isAdminCommand: true,
        shouldFail: false,
    },
    transitionFromDedicatedConfigServer: {skip: requiresMongoS},
    transitionToDedicatedConfigServer: {skip: requiresMongoS},
    transitionToShardedCluster: {skip: isAnInternalCommand},
    unshardCollection: {skip: requiresMongoS},
    untrackUnshardedCollection: {skip: requiresMongoS},
    update: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}));
        },
        command: {update: collName, updates: [{q: {x: 1}, u: {x: 2}}]},
        shouldFail: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    updateRole: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createRole: "foo", privileges: [], roles: []}));
        },
        command: {updateRole: "foo", privileges: []},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    updateSearchIndex: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    updateUser: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {updateUser: "foo", pwd: "bar2"},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    updateZoneKeyRange: {skip: requiresMongoS},
    usersInfo: {
        setUp: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand(
                {createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {usersInfo: "foo"},
        shouldFail: false,
        teardown: function(mongoS, withDirectConnections) {
            assert.commandWorked(withDirectConnections.getDB(dbName).runCommand({dropUser: "foo"}));
        },
    },
    validate: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {validate: collName},
        shouldFail: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    validateDBMetadata: {
        command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}},
        shouldFail: true,
    },
    voteAbortIndexBuild: {skip: isAnInternalCommand},
    voteCommitImportCollection: {skip: isAnInternalCommand},
    voteCommitIndexBuild: {skip: isAnInternalCommand},
    waitForFailPoint: {
        skip: isAnInternalCommand,
    },
    getShardingReady: {skip: isAnInternalCommand},
    whatsmysni: {
        command: {whatsmysni: 1},
        isAdminCommand: true,
        shouldFail: false,
    },
    whatsmyuri: {
        command: {whatsmyuri: 1},
        isAdminCommand: true,
        shouldFail: false,
    }
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

let runCommand = function(
    command, test, mongoS, withDirectConnections, withoutDirectConnections, st) {
    // Skip command if its feature flag is not enabled.
    if (test.checkFeatureFlag) {
        if (!FeatureFlagUtil.isPresentAndEnabled(withDirectConnections.getDB('admin'),
                                                 test.checkFeatureFlag)) {
            jsTestLog("Skipping " + tojson(command) + " because its feature flag is not enabled.");
            return;
        }
    }

    jsTestLog("Testing " + command);

    // If fullScenario is defined, run full setup, command, and teardown in one function.
    if (typeof (test.fullScenario) === "function") {
        test.fullScenario(mongoS, withDirectConnections, withoutDirectConnections);
        return;
    }

    // Otherwise run setUp.
    let cmdObj = test.command;
    if (typeof (test.setUp) === "function") {
        let setUpRes = test.setUp(mongoS, withDirectConnections, withoutDirectConnections);

        // For some commands (such as killSessions) the command requires information that is
        // created during the setUp portion (such as a session ID), so we need to create the
        // command in setUp. We set the command to an empty object in order to indicate that
        // the command created in setUp should be used instead.
        if (Object.keys(test.command).length === 0) {
            cmdObj = setUpRes;
        }
    }

    // Change cmdDb if necessary.
    let cmdDb = test.isAdminCommand ? withoutDirectConnections.getDB('admin')
                                    : withoutDirectConnections.getDB(dbName);

    jsTestLog("Running command: " + tojson(cmdObj));
    if (test.shouldFail) {
        assertCommandOrWriteFailed(
            cmdDb.runCommand(cmdObj), ErrorCodes.Unauthorized, () => tojson(cmdObj));
    } else {
        assert.commandWorked(cmdDb.runCommand(cmdObj), () => tojson(cmdObj));
    }

    // Run test teardown.
    if (typeof (test.teardown) === "function") {
        test.teardown(mongoS, withDirectConnections, withoutDirectConnections);
    }
};

let runAllCommands = function(
    st, mongoS, shardWithDirectConnections, shardWithoutDirectConnections) {
    jsTestLog("Running all commands with direct shard connections");
    // First check that the map contains all available commands.
    let commandsList = AllCommandsTest.checkCommandCoverage(mongoS, allCommands);
    let shardCommandsList =
        AllCommandsTest.checkCommandCoverage(shardWithDirectConnections, allCommands);
    commandsList = new Set(commandsList.concat(shardCommandsList));

    for (const command of commandsList) {
        const test = allCommands[command];

        // Coverage already guaranteed above, but check again just in case.
        assert(test, "Coverage failure: must explicitly define a test for " + command);

        if (test.skip !== undefined) {
            jsTestLog("Skipping " + command + ": " + test.skip);
            continue;
        }

        // Run all commands.
        runCommand(
            command, test, mongoS, shardWithDirectConnections, shardWithoutDirectConnections, st);
    }
};

const st = new ShardingTest({name: jsTestName(), keyFile: "jstests/libs/key1", shards: 2});

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");
const userConn = new Mongo(st.shard0.host);
const userAdminDB = userConn.getDB("admin");

// Establish shard users, one with root privileges and one missing directShardOperations.
shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");
shardAdminDB.createUser({
    user: "user",
    pwd: "y",
    roles: [
        "clusterAdmin",
        "userAdminAnyDatabase",
        "dbAdminAnyDatabase",
        "readWriteAnyDatabase",
        "backup",
        "restore"
    ]
});
assert(userAdminDB.auth("user", "y"), "Authentication failed");

// Increase verbosity so that we always see the direct connection error/warning
assert.commandWorked(shardAdminDB.runCommand(
    {setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 1}, assert: {verbosity: 1}}}));

// Establish mongoS user
const mongoSConn = st.s;
const mongosAdminUser = mongoSConn.getDB('admin');
if (!TestData.configShard) {
    mongosAdminUser.createUser({user: "globalAdmin", pwd: 'a', roles: ["root"]});
    assert(mongosAdminUser.auth("globalAdmin", "a"), "Authentication failed");
} else {
    assert(mongosAdminUser.auth("admin", "x"), "Authentication failed");
}

// Setup database with primary shard set
assert.commandWorked(
    mongosAdminUser.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

runAllCommands(st, mongoSConn, shardConn, userConn);

shardAdminDB.dropUser("user");
mongosAdminUser.logout();
shardAdminDB.logout();

st.stop();
