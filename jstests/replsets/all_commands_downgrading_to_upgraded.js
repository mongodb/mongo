/**
 * This file defines tests for all existing commands and their expected behavior when run against a
 * node that is in the downgrading FCV state.
 *
 * @tags: [
 *   # Tagged as multiversion-incompatible as the list of commands will vary depending on version.
 *   multiversion_incompatible,
 *   # Cannot compact when using the in-memory storage engine.
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

// This will verify the completeness of our map and run all tests.
load("jstests/libs/all_commands_test.js");
load("jstests/libs/fixture_helpers.js");    // For isSharded and isReplSet
load("jstests/libs/feature_flag_util.js");  // For isPresentAndEnabled
load('jstests/replsets/rslib.js');

const name = jsTestName();
const dbName = "alltestsdb";
const collName = "alltestscoll";
const fullNs = dbName + "." + collName;

// Pre-written reasons for skipping a test.
const isAnInternalCommand = "internal command";
const isDeprecated = "deprecated command";
const commandIsDisabledOnLastLTS = "skip command on downgrading fcv";
const requiresParallelShell = "requires parallel shell";
const cannotRunWhileDowngrading = "cannot run command while downgrading";

const allCommands = {
    _addShard: {skip: isAnInternalCommand},
    _clusterQueryWithoutShardKey: {skip: isAnInternalCommand},
    _clusterWriteWithoutShardKey: {skip: isAnInternalCommand},
    _configsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _configsvrAddShard: {skip: isAnInternalCommand},
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
    _configsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _configsvrConfigureCollectionBalancing: {skip: isAnInternalCommand},
    _configsvrCreateDatabase: {skip: isAnInternalCommand},
    _configsvrDropIndexCatalogEntry: {skip: isAnInternalCommand},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isAnInternalCommand},
    _configsvrGetHistoricalPlacement: {skip: isAnInternalCommand},  // TODO SERVER-73029 remove
    _configsvrMoveRange: {skip: isAnInternalCommand},
    _configsvrRefineCollectionShardKey: {skip: isAnInternalCommand},
    _configsvrRemoveChunks: {skip: isAnInternalCommand},
    _configsvrRemoveShard: {skip: isAnInternalCommand},
    _configsvrRemoveShardFromZone: {skip: isAnInternalCommand},
    _configsvrRemoveTags: {skip: isAnInternalCommand},
    _configsvrRepairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    _configsvrRenameCollectionMetadata: {skip: isAnInternalCommand},
    _configsvrReshardCollection: {skip: isAnInternalCommand},
    _configsvrRunRestore: {skip: isAnInternalCommand},
    _configsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _configsvrSetClusterParameter: {skip: isAnInternalCommand},
    _configsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _configsvrTransitionToCatalogShard: {skip: isAnInternalCommand},
    _configsvrTransitionToDedicatedConfigServer: {skip: isAnInternalCommand},
    _configsvrUpdateZoneKeyRange: {skip: isAnInternalCommand},
    _flushDatabaseCacheUpdates: {skip: isAnInternalCommand},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: isAnInternalCommand},
    _flushReshardingStateChange: {skip: isAnInternalCommand},
    _flushRoutingTableCacheUpdates: {skip: isAnInternalCommand},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: isAnInternalCommand},
    _getAuditConfigGeneration: {skip: isAnInternalCommand},
    _getNextSessionMods: {skip: isAnInternalCommand},
    _getUserCacheGeneration: {skip: isAnInternalCommand},
    _hashBSONElement: {skip: isAnInternalCommand},
    _isSelf: {skip: isAnInternalCommand},
    _killOperations: {skip: isAnInternalCommand},
    _mergeAuthzCollections: {skip: isAnInternalCommand},
    _migrateClone: {skip: isAnInternalCommand},
    _movePrimaryRecipientAbortMigration: {skip: isAnInternalCommand},
    _movePrimaryRecipientForgetMigration: {skip: isAnInternalCommand},
    _movePrimaryRecipientSyncData: {skip: isAnInternalCommand},
    _recvChunkAbort: {skip: isAnInternalCommand},
    _recvChunkCommit: {skip: isAnInternalCommand},
    _recvChunkReleaseCritSec: {skip: isAnInternalCommand},
    _recvChunkStart: {skip: isAnInternalCommand},
    _recvChunkStatus: {skip: isAnInternalCommand},
    _refreshQueryAnalyzerConfiguration: {skip: isAnInternalCommand},
    _shardsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _shardsvrCleanupReshardCollection: {skip: isAnInternalCommand},
    _shardsvrCloneCatalogData: {skip: isAnInternalCommand},
    _shardsvrCompactStructuredEncryptionData: {skip: isAnInternalCommand},
    _shardsvrRegisterIndex: {skip: isAnInternalCommand},
    _shardsvrCommitIndexParticipant: {skip: isAnInternalCommand},
    _shardsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _shardsvrDropCollection: {skip: isAnInternalCommand},
    _shardsvrCreateCollection: {skip: isAnInternalCommand},
    _shardsvrCreateGlobalIndex: {skip: isAnInternalCommand},
    // TODO SERVER-74324: deprecate _shardsvrDropCollectionIfUUIDNotMatching after 7.0 is lastLTS.
    _shardsvrDropCollectionIfUUIDNotMatching: {skip: isAnInternalCommand},
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: isAnInternalCommand},
    _shardsvrDropCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrDropGlobalIndex: {skip: isAnInternalCommand},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: isAnInternalCommand},
    _shardsvrDropIndexes: {skip: isAnInternalCommand},
    _shardsvrCreateCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrGetStatsForBalancing: {skip: isAnInternalCommand},
    _shardsvrInsertGlobalIndexKey: {skip: isAnInternalCommand},
    _shardsvrDeleteGlobalIndexKey: {skip: isAnInternalCommand},
    _shardsvrWriteGlobalIndexKeys: {skip: isAnInternalCommand},
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
    _shardsvrDropDatabase: {skip: isAnInternalCommand},
    _shardsvrDropDatabaseParticipant: {skip: isAnInternalCommand},
    _shardsvrReshardCollection: {skip: isAnInternalCommand},
    _shardsvrReshardingOperationTime: {skip: isAnInternalCommand},
    _shardsvrRefineCollectionShardKey: {skip: isAnInternalCommand},
    _shardsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _shardsvrSetClusterParameter: {skip: isAnInternalCommand},
    _shardsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _shardsvrUnregisterIndex: {skip: isAnInternalCommand},
    _shardsvrValidateShardKeyCandidate: {skip: isAnInternalCommand},
    _shardsvrCollMod: {skip: isAnInternalCommand},
    _shardsvrCollModParticipant: {skip: isAnInternalCommand},
    _shardsvrParticipantBlock: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistency: {skip: isAnInternalCommand},
    _shardsvrCheckMetadataConsistencyParticipant: {skip: isAnInternalCommand},
    _startStreamProcessor: {skip: isAnInternalCommand},
    streams_startStreamSample: {skip: isAnInternalCommand},
    streams_getMoreStreamSample: {skip: isAnInternalCommand},
    streams_testOnlyInsert: {skip: isAnInternalCommand},
    _transferMods: {skip: isAnInternalCommand},
    _vectorClockPersist: {skip: isAnInternalCommand},
    abortReshardCollection: {
        // Skipping command because it requires testing through a parallel shell.
        skip: requiresParallelShell,
    },
    abortTransaction: {
        doesNotRunOnStandalone: true,
        fullScenario: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({create: collName, writeConcern: {w: 'majority'}}));

            let _lsid = UUID();
            // Start the transaction.
            assert.commandWorked(conn.getDB(dbName).runCommand({
                insert: collName,
                documents: [{_id: ObjectId()}],
                lsid: {id: _lsid},
                stmtIds: [NumberInt(0)],
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
            }));

            assert.commandWorked(conn.getDB('admin').runCommand({
                abortTransaction: 1,
                txnNumber: NumberLong(0),
                autocommit: false,
                lsid: {id: _lsid},
            }));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        }
    },
    addShard: {
        skip: "cannot add shard while in downgrading FCV state",
    },
    addShardToZone: {
        isShardedOnly: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(
                conn.adminCommand({addShardToZone: fixture.shard0.shardName, zone: 'x'}));
            assert.commandWorked(
                conn.adminCommand({removeShardFromZone: fixture.shard0.shardName, zone: 'x'}));
        }
    },
    aggregate: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
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
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    analyze: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {analyze: collName},
        expectFailure: true,
        expectedErrorCode: [
            6660400,
            6765500
        ],  // Analyze command requires common query framework feature flag to be enabled.
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    analyzeShardKey: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-67966: Remove check when this feature flag is removed.
        checkFeatureFlag: "AnalyzeShardKey",
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB('admin').runCommand({shardCollection: fullNs, key: {_id: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {analyzeShardKey: fullNs, key: {_id: 1}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isShardedOnly: true,
        isAdminCommand: true,
    },
    appendOplogNote: {
        command: {appendOplogNote: 1, data: {a: 1}},
        isAdminCommand: true,
        doesNotRunOnStandalone: true,
    },
    applyOps: {
        command: {applyOps: []},
        isAdminCommand: true,
        isShardSvrOnly: true,
    },
    authenticate: {
        // Skipping command because it requires additional authentication setup.
        skip: "requires additional authentication setup"
    },
    autoSplitVector: {skip: isAnInternalCommand},
    balancerCollectionStatus: {
        command: {balancerCollectionStatus: fullNs},
        isShardedOnly: true,
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB('admin').runCommand({shardCollection: fullNs, key: {_id: 1}}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    balancerStart: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand({balancerStop: 1}));
        },
        command: {balancerStart: 1},
        isShardedOnly: true,
        isAdminCommand: true
    },
    balancerStatus: {command: {balancerStatus: 1}, isShardedOnly: true, isAdminCommand: true},
    balancerStop: {
        command: {balancerStop: 1},
        isShardedOnly: true,
        isAdminCommand: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand({balancerStart: 1}));
        },
    },
    buildInfo: {
        command: {buildInfo: 1},
        isAdminCommand: true,
    },
    bulkWrite: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-67711: Remove check when this feature flag is removed.
        checkFeatureFlag: "BulkWriteCommand",
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            bulkWrite: 1,
            ops: [
                {insert: 0, document: {skey: "MongoDB"}},
                {insert: 0, document: {skey: "MongoDB"}}
            ],
            nsInfo: [{ns: fullNs}]
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    captrunc: {
        skip: isAnInternalCommand,
    },
    checkMetadataConsistency: {
        isAdminCommand: true,
        isShardedOnly: true,
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-70396: Remove check when this feature flag is removed.
        checkFeatureFlag: "CheckMetadataConsistency",
        command: {checkMetadataConsistency: 1},
    },
    checkShardingIndex: {
        setUp: function(conn, fixture) {
            assert.commandWorked(fixture.shard0.getDB(dbName).runCommand({create: collName}));
            const f = fixture.shard0.getCollection(fullNs);
            f.createIndex({x: 1, y: 1});
        },
        command: {checkShardingIndex: fullNs, keyPattern: {x: 1, y: 1}},
        isShardedOnly: true,
        isShardSvrOnly: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        }
    },
    cleanupOrphaned: {
        setUp: function(conn) {
            // This would not actually create any orphaned data so the command will be a noop, but
            // this will be tested through the sharding FCV upgrade/downgrade passthrough.
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {cleanupOrphaned: fullNs},
        isShardedOnly: true,
        isShardSvrOnly: true,
        isAdminCommand: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    cleanupReshardCollection: {
        // Skipping command because it requires additional setup through a failed resharding
        // operation.
        skip: "requires additional setup through a failed resharding operation",
    },
    cleanupStructuredEncryptionData: {skip: "requires additional encrypted collection setup"},
    clearJumboFlag: {
        isShardedOnly: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));

            assert.commandWorked(conn.adminCommand({split: fullNs, middle: {a: 5}}));

            // Create sufficient documents to create a jumbo chunk, and use the same shard key in
            // all of
            // them so that the chunk cannot be split.
            const largeString = 'X'.repeat(1024 * 1024);
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(
                    conn.getCollection(fullNs).insert({a: 0, big: largeString, i: i}));
            }

            assert.commandWorked(conn.adminCommand({clearJumboFlag: fullNs, find: {a: 0}}));

            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    clearLog: {
        command: {clearLog: 'global'},
        isAdminCommand: true,
    },
    cloneCollectionAsCapped: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {
            cloneCollectionAsCapped: collName,
            toCollection: collName + "2",
            size: 10 * 1024 * 1024
        },
        doesNotRunOnMongos: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName + "2"}));
        },
    },
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterCount: {skip: "already tested by 'count' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {collMod: collName, validator: {}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    collStats: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {aggregate: collName, pipeline: [{$collStats: {count: {}}}], cursor: {}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    commitReshardCollection: {
        skip: requiresParallelShell,
    },
    commitTransaction: {
        doesNotRunOnStandalone: true,
        fullScenario: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({create: collName, writeConcern: {w: 'majority'}}));
            let _lsid = UUID();
            // Start the transaction.
            assert.commandWorked(conn.getDB(dbName).runCommand({
                insert: collName,
                documents: [{_id: ObjectId()}],
                lsid: {id: _lsid},
                stmtIds: [NumberInt(0)],
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
            }));

            assert.commandWorked(conn.getDB("admin").runCommand({
                commitTransaction: 1,
                txnNumber: NumberLong(0),
                autocommit: false,
                lsid: {id: _lsid},
            }));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    compact: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {compact: collName, force: true},
        isReplSetOnly: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    compactStructuredEncryptionData: {skip: "requires additional encrypted collection setup"},
    configureFailPoint: {skip: isAnInternalCommand},
    configureCollectionBalancing: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB('admin').runCommand({shardCollection: fullNs, key: {_id: 1}}));
        },
        command: {configureCollectionBalancing: fullNs, chunkSize: 1},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isShardedOnly: true,
        isAdminCommand: true,
    },
    configureQueryAnalyzer: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-67966: Remove check when this feature flag is removed.
        checkFeatureFlag: "AnalyzeShardKey",
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {configureQueryAnalyzer: fullNs, mode: "full", sampleRate: 1},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isAdminCommand: true,
        isShardedOnly: true,
    },
    connPoolStats: {
        isAdminCommand: true,
        command: {connPoolStats: 1},
    },
    connPoolSync: {isAdminCommand: true, command: {connPoolSync: 1}},
    connectionStatus: {isAdminCommand: true, command: {connectionStatus: 1}},
    convertToCapped: {
        command: {convertToCapped: collName, size: 10 * 1024 * 1024},
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    coordinateCommitTransaction: {skip: isAnInternalCommand},
    count: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {count: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    cpuload: {skip: isAnInternalCommand},
    create: {
        command: {create: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    createIndexes: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}));
        },
        command: {createIndexes: collName, indexes: [{key: {x: 1}, name: "foo"}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    createRole: {
        command: {createRole: "foo", privileges: [], roles: []},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    createSearchIndexes: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    createUser: {
        command: {createUser: "foo", pwd: "bar", roles: []},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    currentOp: {
        command: {currentOp: 1},
        isAdminCommand: true,
    },
    dataSize: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {dataSize: fullNs},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    dbCheck: {command: {dbCheck: 1}, isShardSvrOnly: true},
    dbHash: {
        command: {dbHash: 1},
        isShardSvrOnly: true,
    },
    dbStats: {
        command: {dbStats: 1},
    },
    delete: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {delete: collName, deletes: [{q: {x: 1}, limit: 1}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    distinct: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {distinct: collName, key: "a"},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    donorAbortMigration: {skip: isAnInternalCommand},
    donorForgetMigration: {skip: isAnInternalCommand},
    donorStartMigration: {skip: isAnInternalCommand},
    abortShardSplit: {skip: isAnInternalCommand},
    commitShardSplit: {skip: isAnInternalCommand},
    forgetShardSplit: {skip: isAnInternalCommand},
    drop: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {drop: collName},
    },
    dropAllRolesFromDatabase: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
        },
        command: {dropAllRolesFromDatabase: 1},
    },
    dropAllUsersFromDatabase: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {dropAllUsersFromDatabase: 1},
    },
    dropConnections: {
        // This will be tested in FCV upgrade/downgrade passthroughs through tests in the replsets
        // and sharding directories.
        skip: "requires additional setup to reconfig and add/remove nodes",
    },
    dropDatabase: {
        command: {dropDatabase: 1},
    },
    dropIndexes: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}));
            assert.commandWorked(conn.getDB(dbName).runCommand(
                {createIndexes: collName, indexes: [{key: {x: 1}, name: "foo"}]}));
        },
        command: {dropIndexes: collName, index: {x: 1}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    dropRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
        },
        command: {dropRole: "foo"},
    },
    dropSearchIndex: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    dropUser: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {dropUser: "foo"},
    },
    echo: {command: {echo: 1}},
    emptycapped: {
        command: {emptycapped: collName},
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        doesNotRunOnMongos: true,
    },
    enableSharding: {
        isShardedOnly: true,
        isAdminCommand: true,
        command: {enableSharding: dbName},
    },
    endSessions: {skip: "tested in startSession"},
    explain: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {explain: {count: collName}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    features: {command: {features: 1}},
    filemd5: {
        setUp: function(conn) {
            const f = conn.getCollection(dbName + ".fs.chunks");
            assert.commandWorked(f.createIndex({files_id: 1, n: 1}));
        },
        command: {filemd5: 1, root: "fs"},
    },
    find: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {find: collName, filter: {a: 1}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    findAndModify: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}));
        },
        command: {findAndModify: collName, query: {x: 1}, update: {$set: {x: 2}}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        }
    },
    flushRouterConfig: {isShardedOnly: true, isAdminCommand: true, command: {flushRouterConfig: 1}},
    fsync: {
        command: {fsync: 1},
        isAdminCommand: true,
        doesNotRunOnMongos: true,
    },
    fsyncUnlock: {
        command: {fsyncUnlock: 1},
        isAdminCommand: true,
        doesNotRunOnMongos: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand({fsync: 1, lock: 1}));
        }
    },
    getAuditConfig: {
        isAdminCommand: true,
        command: {getAuditConfig: 1},
    },
    getChangeStreamState: {
        isAdminCommand: true,
        doesNotRunOnMongos: true,
        command: {getChangeStreamState: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.CommandNotSupported  // only supported on serverless.
    },
    getClusterParameter: {
        isAdminCommand: true,
        command: {getClusterParameter: "changeStreamOptions"},
        doesNotRunOnStandalone: true,
    },
    getCmdLineOpts: {
        isAdminCommand: true,
        command: {getCmdLineOpts: 1},
    },
    getDatabaseVersion: {
        isAdminCommand: true,
        command: {getDatabaseVersion: dbName},
        isShardedOnly: true,
        isShardSvrOnly: true,
    },
    getDefaultRWConcern: {
        isAdminCommand: true,
        command: {getDefaultRWConcern: 1},
        doesNotRunOnStandalone: true,
    },
    getDiagnosticData: {
        isAdminCommand: true,
        command: {getDiagnosticData: 1},
    },
    getFreeMonitoringStatus: {
        isAdminCommand: true,
        command: {getFreeMonitoringStatus: 1},
        doesNotRunOnMongos: true,
    },
    getLog: {
        isAdminCommand: true,
        command: {getLog: "global"},
    },
    getMore: {
        fullScenario: function(conn) {
            const db = conn.getDB(dbName);
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }

            const res = db.runCommand({find: collName, batchSize: 1});
            assert.commandWorked(res);
            assert.commandWorked(
                db.runCommand({getMore: NumberLong(res.cursor.id), collection: collName}));

            assert.commandWorked(
                db.runCommand({killCursors: collName, cursors: [NumberLong(res.cursor.id)]}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    getParameter: {isAdminCommand: true, command: {getParameter: 1, logLevel: 1}},
    getQueryableEncryptionCountInfo: {skip: isAnInternalCommand},
    getShardMap: {
        isAdminCommand: true,
        command: {getShardMap: 1},
        isShardedOnly: true,
    },
    getShardVersion: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {getShardVersion: dbName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isShardedOnly: true,
        isAdminCommand: true,
    },
    godinsert: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        command: {godinsert: collName, obj: {_id: 0, a: 0}},
        doesNotRunOnMongos: true,
    },
    grantPrivilegesToRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            grantPrivilegesToRole: "foo",
            privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        }
    },
    grantRolesToRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "bar", privileges: [], roles: []}));
        },
        command: {grantRolesToRole: "foo", roles: [{role: "bar", db: dbName}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "bar"}));
        }
    },
    grantRolesToUser: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {grantRolesToUser: "foo", roles: [{role: "foo", db: dbName}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    hello: {
        isAdminCommand: true,
        command: {hello: 1},
    },
    hostInfo: {isAdminCommand: true, command: {hostInfo: 1}},
    httpClientRequest: {skip: isAnInternalCommand},
    exportCollection: {skip: isAnInternalCommand},
    importCollection: {skip: isAnInternalCommand},
    insert: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        command: {insert: collName, documents: [{_id: ObjectId()}]},
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: isAnInternalCommand},
    invalidateUserCache: {
        isAdminCommand: 1,
        command: {invalidateUserCache: 1},
    },
    isdbgrid: {
        isAdminCommand: 1,
        command: {isdbgrid: 1},
        isShardedOnly: true,
    },
    isMaster: {
        isAdminCommand: 1,
        command: {isMaster: 1},
    },
    killAllSessions: {
        command: {killAllSessions: []},
    },
    killAllSessionsByPattern: {
        command: {killAllSessionsByPattern: []},
    },
    killCursors: {
        setUp: function(conn) {
            const db = conn.getDB(dbName);
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }

            const res = db.runCommand({find: collName, batchSize: 1});
            const cmdObj = {killCursors: collName, cursors: [NumberLong(res.cursor.id)]};
            return cmdObj;
        },
        // This command requires information that is created during the setUp portion (a cursor ID),
        // so we need to create the command in setUp. We set the command to an empty object in order
        // to indicate that the command created in setUp should be used instead.
        command: {},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    killOp: {
        // This will be tested in FCV upgrade/downgrade passthroughs through tests in the replsets
        // directory.
        skip: requiresParallelShell
    },
    killSessions: {
        setUp: function(conn) {
            const admin = conn.getDB("admin");
            const result = admin.runCommand({startSession: 1});
            assert.commandWorked(result, "failed to startSession");
            const lsid = result.id;
            return {killSessions: [lsid]};
        },
        // This command requires information that is created during the setUp portion (a session
        // ID),
        // so we need to create the command in setUp. We set the command to an empty object in order
        // to indicate that the command created in setUp should be used instead.
        command: {},
    },
    listCollections: {
        command: {listCollections: 1},
    },
    listCommands: {command: {listCommands: 1}},
    listDatabases: {
        command: {listDatabases: 1},
        isAdminCommand: true,
    },
    listDatabasesForAllTenants: {
        skip: isAnInternalCommand,
    },
    listIndexes: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {listIndexes: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    listSearchIndexes: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    listShards: {
        isShardedOnly: true,
        isAdminCommand: true,
        command: {listShards: 1},
    },
    lockInfo: {
        isAdminCommand: true,
        command: {lockInfo: 1},
        doesNotRunOnMongos: true,
    },
    logApplicationMessage: {
        isAdminCommand: true,
        command: {logApplicationMessage: "hello"},
    },
    logMessage: {
        skip: isAnInternalCommand,
    },
    logRotate: {
        isAdminCommand: true,
        command: {logRotate: 1},
    },
    logout: {
        skip: "requires additional authentication setup",
    },
    makeSnapshot: {
        isAdminCommand: true,
        command: {makeSnapshot: 1},
        doesNotRunOnMongos: true,
    },
    mapReduce: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {
            mapReduce: collName,
            map: function() {},
            reduce: function(key, vals) {},
            out: {inline: 1}
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    mergeAllChunksOnShard: {
        isShardedOnly: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
            assert.commandWorked(conn.adminCommand({split: fullNs, middle: {a: 5}}));
            assert.commandWorked(conn.adminCommand({
                moveChunk: fullNs,
                find: {a: 1},
                to: fixture.shard0.shardName,
                _waitForDelete: true
            }));
            assert.commandWorked(conn.adminCommand({
                moveChunk: fullNs,
                find: {a: 9},
                to: fixture.shard0.shardName,
                _waitForDelete: true
            }));
            assert.commandWorked(conn.adminCommand(
                {mergeAllChunksOnShard: fullNs, shard: fixture.shard0.shardName}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    mergeChunks: {
        isShardedOnly: true,
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {_id: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
            assert.commandWorked(conn.adminCommand({split: fullNs, middle: {_id: 5}}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        command: {mergeChunks: fullNs, bounds: [{_id: MinKey}, {_id: MaxKey}]},
    },
    moveChunk: {
        isShardedOnly: true,
        isAdminCommand: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
            assert.commandWorked(conn.adminCommand({split: fullNs, middle: {a: 5}}));
            assert.commandWorked(conn.adminCommand({
                moveChunk: fullNs,
                find: {a: 1},
                to: fixture.shard0.shardName,
                _waitForDelete: true
            }));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    movePrimary: {
        skip: cannotRunWhileDowngrading,
    },
    moveRange: {
        isShardedOnly: true,
        isAdminCommand: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
            assert.commandWorked(conn.adminCommand({split: fullNs, middle: {a: 5}}));
            assert.commandWorked(conn.adminCommand(
                {moveRange: fullNs, min: {a: 1}, toShard: fixture.shard0.shardName}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    multicast: {
        command: {multicast: {ping: 0}},
        isShardedOnly: true,
        isAdminCommand: true,
    },
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
    ping: {isAdminCommand: true, command: {ping: 1}},
    planCacheClear: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheClear: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheClearFilters: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheClearFilters: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheListFilters: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheListFilters: collName},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    planCacheSetFilter: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    prepareTransaction: {skip: isAnInternalCommand},
    profile: {
        doesNotRunOnMongos: true,
        isAdminCommand: true,
        command: {profile: 2},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand({profile: 0}));
        },
    },
    reapLogicalSessionCacheNow: {
        isAdminCommand: true,
        command: {reapLogicalSessionCacheNow: 1},
    },
    recipientForgetMigration: {skip: isAnInternalCommand},
    recipientSyncData: {skip: isAnInternalCommand},
    recipientVoteImportedFiles: {skip: isAnInternalCommand},
    refineCollectionShardKey: {
        isShardedOnly: true,
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i, b: i}));
            }
            const testColl = conn.getCollection(fullNs);
            assert.commandWorked(testColl.createIndex({a: 1, b: 1}));
        },
        command: {refineCollectionShardKey: fullNs, key: {a: 1, b: 1}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    refreshLogicalSessionCacheNow: {
        command: {refreshLogicalSessionCacheNow: 1},
        isAdminCommand: true,
    },
    refreshSessions: {
        setUp: function(conn) {
            const admin = conn.getDB("admin");
            const result = admin.runCommand({startSession: 1});
            assert.commandWorked(result, "failed to startSession");
            const lsid = result.id;
            return {refreshSessions: [lsid]};
        },
        // This command requires information that is created during the setUp portion (a session
        // ID),
        // so we need to create the command in setUp. We set the command to an empty object in order
        // to indicate that the command created in setUp should be used instead.
        command: {},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB("admin").runCommand({killAllSessions: []}));
        }
    },
    reIndex: {
        skip: isDeprecated,
    },
    removeShard: {
        // This will be tested in FCV upgrade/downgrade passthroughs in the sharding
        // directory.
        skip: "cannot add shard while in downgrading FCV state",
    },
    removeShardFromZone: {
        skip: "tested in addShardToZone",
    },
    renameCollection: {
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {renameCollection: fullNs, to: fullNs + "2"},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName + "2"}));
        }
    },
    repairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    replSetAbortPrimaryCatchUp: {
        // This will be tested in FCV upgrade/downgrade passthroughs through the replsets directory.
        skip: "requires changing primary connection",
    },
    replSetFreeze: {
        isReplSetOnly: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(
                fixture.getSecondary().getDB("admin").runCommand({replSetFreeze: 1}));
            assert.commandWorked(
                fixture.getSecondary().getDB("admin").runCommand({replSetFreeze: 0}));
        }
    },
    replSetGetConfig: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetConfig: 1}},
    replSetGetRBID: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetRBID: 1}},
    replSetGetStatus: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetStatus: 1}},
    replSetHeartbeat: {skip: isAnInternalCommand},
    replSetInitiate: {
        // This will be tested in FCV upgrade/downgrade passthroughs through the replsets directory.
        skip: "requires starting a new replica set"
    },
    replSetMaintenance: {
        isReplSetOnly: true,
        fullScenario: function(conn, fixture) {
            assert.commandWorked(
                fixture.getSecondary().getDB("admin").runCommand({replSetMaintenance: 1}));
            assert.commandWorked(
                fixture.getSecondary().getDB("admin").runCommand({replSetMaintenance: 0}));
        }
    },
    replSetReconfig: {
        isReplSetOnly: true,
        fullScenario: function(conn, fixture) {
            let config = fixture.getReplSetConfigFromNode();
            config.version++;
            assert.commandWorked(conn.getDB("admin").runCommand({replSetReconfig: config}));
        }
    },
    replSetRequestVotes: {skip: isAnInternalCommand},
    replSetStepDown: {
        // This will be tested in FCV upgrade/downgrade passthroughs through tests in the replsets
        // directory.
        skip: "requires changing primary connection",
    },
    replSetStepUp: {
        // This will be tested in FCV upgrade/downgrade passthroughs through tests in the replsets
        // directory.
        skip: "requires changing primary connection",
    },
    replSetSyncFrom: {
        isReplSetOnly: true,
        fullScenario: function(conn, fixture) {
            const secondary1 = fixture.getSecondaries()[0];
            const secondary2 = fixture.getSecondaries()[1];
            assert.commandWorked(secondary2.adminCommand({replSetSyncFrom: secondary1.name}));
            // Sync from primary again.
            assert.commandWorked(secondary2.adminCommand({replSetSyncFrom: conn.name}));
        }
    },
    replSetTest: {skip: isAnInternalCommand},
    replSetTestEgress: {skip: isAnInternalCommand},
    replSetUpdatePosition: {skip: isAnInternalCommand},
    replSetResizeOplog: {
        isReplSetOnly: true,
        isAdminCommand: true,
        command: {replSetResizeOplog: 1, minRetentionHours: 1},
    },
    reshardCollection: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        isShardedOnly: true,
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            const testColl = conn.getCollection(fullNs);
            assert.commandWorked(
                conn.getDB('admin').runCommand({shardCollection: fullNs, key: {_id: 1}}));

            // Build an index on the collection to support the resharding operation.
            assert.commandWorked(testColl.createIndex({a: 1}));

            // Insert some documents that will be resharded.
            assert.commandWorked(testColl.insert({_id: 0, a: 0}));
            assert.commandWorked(testColl.insert({_id: 1, a: 1}));
        },
        command: {
            reshardCollection: fullNs,
            key: {a: 1},
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    revokePrivilegesFromRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(conn.getDB(dbName).runCommand({
                createRole: "foo",
                privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}],
                roles: [],
            }));
        },
        command: {
            revokePrivilegesFromRole: "foo",
            privileges: [{resource: {db: dbName, collection: collName}, actions: ["find"]}]
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        }
    },
    revokeRolesFromRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "bar", privileges: [], roles: []}));
            assert.commandWorked(conn.getDB(dbName).runCommand({
                createRole: "foo",
                privileges: [],
                roles: [{role: "bar", db: dbName}],
            }));
        },
        command: {revokeRolesFromRole: "foo", roles: [{role: "foo", db: dbName}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "bar"}));
        }
    },
    revokeRolesFromUser: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
            assert.commandWorked(conn.getDB(dbName).runCommand({
                createUser: "foo",
                pwd: "bar",
                roles: [{role: "foo", db: dbName}],
            }));
        },
        command: {revokeRolesFromUser: "foo", roles: [{role: "foo", db: dbName}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
            assert.commandWorked(conn.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    rolesInfo: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
        },
        command: {rolesInfo: 1},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    rotateCertificates: {skip: "requires additional authentication setup"},
    saslContinue: {skip: "requires additional authentication setup"},
    saslStart: {skip: "requires additional authentication setup"},
    serverStatus: {
        isAdminCommand: true,
        command: {serverStatus: 1},
    },
    setAuditConfig: {skip: "requires additional audit/authentication setup"},
    setAllowMigrations: {
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {_id: 1}}));
        },
        command: {setAllowMigrations: fullNs, allowMigrations: true},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isShardedOnly: true,
    },
    setCommittedSnapshot: {skip: isAnInternalCommand},
    setDefaultRWConcern: {
        command:
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}},
        teardown: function(conn) {
            assert.commandWorked(conn.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "majority"},
                writeConcern: {w: "majority"}
            }));
        },
        isAdminCommand: true,
        doesNotRunOnStandalone: true,
    },
    setIndexCommitQuorum: {skip: requiresParallelShell},
    setFeatureCompatibilityVersion: {skip: "is tested through this test"},
    setFreeMonitoring: {
        skip: "requires cloudFreeMonitoringEndpointURL setup",
    },
    setProfilingFilterGlobally: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        command: {setProfilingFilterGlobally: 1, filter: {nreturned: 0}},
        expectFailure: true,
        expectedErrorCode:
            7283301  // setProfilingFilterGlobally command requires query knob to be enabled.
    },
    setParameter: {
        command: {setParameter: 1, requireApiVersion: true},
        isAdminCommand: true,
        teardown: function(conn) {
            assert.commandWorked(conn.getDB('admin').runCommand(
                {setParameter: 1, requireApiVersion: false, apiVersion: "1"}));
        }
    },
    setChangeStreamState: {
        isAdminCommand: true,
        command: {setChangeStreamState: 1, enabled: true},
        doesNotRunOnMongos: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.CommandNotSupported  // only supported on serverless.
    },
    setClusterParameter: {
        isAdminCommand: true,
        doesNotRunOnStandalone: true,
        command: {setClusterParameter: {testIntClusterParameter: {intData: 2022}}}
    },
    setUserWriteBlockMode: {
        command: {setUserWriteBlockMode: 1, global: true},
        teardown: function(conn) {
            assert.commandWorked(
                conn.getDB('admin').runCommand({setUserWriteBlockMode: 1, global: false}));
        },
        doesNotRunOnStandalone: true,
    },
    shardCollection: {
        isShardedOnly: true,
        isAdminCommand: true,
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        command: {shardCollection: fullNs, key: {_id: 1}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    shardingState: {
        isAdminCommand: true,
        command: {shardingState: 1},
        isShardSvrOnly: true,
    },
    shutdown: {skip: "tested in multiVersion/genericSetFCVUsage/restart_during_downgrading_fcv.js"},
    sleep: {skip: isAnInternalCommand},
    split: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {_id: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {split: fullNs, middle: {_id: 5}},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        isAdminCommand: true,
        isShardedOnly: true,
    },
    splitChunk: {skip: isAnInternalCommand},
    splitVector: {skip: isAnInternalCommand},
    stageDebug: {skip: isAnInternalCommand},
    startRecordingTraffic: {
        // Skipping command because it requires an actual file path for recording traffic to.
        skip: "requires an actual file path to record traffic to",
    },
    startSession: {
        fullScenario: function(conn, fixture) {
            const res = conn.adminCommand({startSession: 1});
            assert.commandWorked(res);
            assert.commandWorked(conn.adminCommand({endSessions: [res.id]}));
        }
    },
    stopRecordingTraffic: {
        // Skipping command because it requires an actual file path for recording traffic to.
        skip: "requires an actual file path to record traffic to",
    },
    testDeprecation: {skip: isAnInternalCommand},
    testDeprecationInVersion2: {skip: isAnInternalCommand},
    testInternalTransactions: {skip: isAnInternalCommand},
    testRemoval: {skip: isAnInternalCommand},
    testReshardCloneCollection: {skip: isAnInternalCommand},
    testVersions1And2: {skip: isAnInternalCommand},
    testVersion2: {skip: isAnInternalCommand},
    top: {
        command: {top: 1},
        isAdminCommand: true,
        doesNotRunOnMongos: true,
    },
    transitionToCatalogShard: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-66060: Remove check when this feature flag is removed.
        checkFeatureFlag: "CatalogShard",
        command: {transitionToCatalogShard: 1},
        isShardedOnly: true,
        isAdminCommand: true,
    },
    transitionToDedicatedConfigServer: {
        // TODO SERVER-74867: Remove the skip once 7.0 is lastLTS.
        skip: commandIsDisabledOnLastLTS,
        // TODO SERVER-66060: Remove check when this feature flag is removed.
        checkFeatureFlag: "CatalogShard",
        command: {transitionToDedicatedConfigServer: 1},
        isShardedOnly: true,
        isAdminCommand: true,
    },
    update: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}));
        },
        command: {update: collName, updates: [{q: {x: 1}, u: {x: 2}}]},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    updateRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
        },
        command: {updateRole: "foo", privileges: []},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropRole: "foo"}));
        }
    },
    updateSearchIndex: {
        // Skipping command as it requires additional Mongot mock setup (and is an enterprise
        // feature).
        skip: "requires mongot mock setup",
    },
    updateUser: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {updateUser: "foo", pwd: "bar2"},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropUser: "foo"}));
        }
    },
    updateZoneKeyRange: {
        isAdminCommand: true,
        isShardedOnly: true,
        setUp: function(conn, fixture) {
            assert.commandWorked(
                conn.adminCommand({addShardToZone: fixture.shard0.shardName, zone: 'zone0'}));
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
            assert.commandWorked(
                conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {a: 1}}));
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
            }
        },
        command: {updateZoneKeyRange: fullNs, min: {a: MinKey}, max: {a: 5}, zone: 'zone0'},
        teardown: function(conn, fixture) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
            assert.commandWorked(
                conn.adminCommand({removeShardFromZone: fixture.shard0.shardName, zone: 'zone0'}));
        }
    },
    usersInfo: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {usersInfo: "foo"},
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({dropUser: "foo"}));
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
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    validateDBMetadata: {
        command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}},
    },
    voteAbortIndexBuild: {skip: isAnInternalCommand},
    voteCommitImportCollection: {skip: isAnInternalCommand},
    voteCommitIndexBuild: {skip: isAnInternalCommand},
    waitForFailPoint: {
        skip: isAnInternalCommand,
    },
    waitForOngoingChunkSplits: {
        command: {waitForOngoingChunkSplits: 1},
        isShardedOnly: true,
        isShardSvrOnly: true,
    },
    whatsmysni: {
        command: {whatsmysni: 1},
        isAdminCommand: true,
        doesNotRunOnMongos: true,
    },
    whatsmyuri: {
        command: {whatsmyuri: 1},
        isAdminCommand: true,
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

let runAllCommands = function(command, test, conn, fixture) {
    let cmdDb = conn.getDB(dbName);
    const isShardedCluster = isMongos(cmdDb);
    const isReplSet = FixtureHelpers.isReplSet(cmdDb);

    // Skip command if it does not run on this type of cluster.
    if (test.isShardedOnly && !isShardedCluster) {
        jsTestLog("Skipping " + tojson(command) + " because it is for sharded clusters only");
        return;
    }
    if (test.isReplSetOnly && !isReplSet) {
        jsTestLog("Skipping " + tojson(command) + " because it is for replica sets only");
        return;
    }
    if (test.isStandaloneOnly && (isShardedCluster || isReplSet)) {
        jsTestLog("Skipping " + tojson(command) + " because it is for standalones only");
        return;
    }
    if (test.doesNotRunOnStandalone && !(isShardedCluster || isReplSet)) {
        jsTestLog("Skipping " + tojson(command) + " because it does not run on standalones");
        return;
    }
    if (test.doesNotRunOnMongos && isShardedCluster) {
        jsTestLog("Skipping " + tojson(command) + " because it does not run on mongos");
        return;
    }

    // Skip command if its feature flag is not enabled.
    if (test.checkFeatureFlag) {
        if (isShardedCluster) {
            if (!FeatureFlagUtil.isPresentAndEnabled(fixture.configRS.getPrimary().getDB('admin'),
                                                     test.checkFeatureFlag)) {
                jsTestLog("Skipping " + tojson(command) +
                          " because its feature flag is not enabled.");
                return;
            }
        } else {
            if (!FeatureFlagUtil.isPresentAndEnabled(cmdDb.getSiblingDB("admin"),
                                                     test.checkFeatureFlag)) {
                jsTestLog("Skipping " + tojson(command) +
                          " because its feature flag is not enabled.");
                return;
            }
        }
    }

    jsTestLog("Testing " + command);

    // If fullScenario is defined, run full setup, command, and teardown in one function.
    if (typeof (test.fullScenario) === "function") {
        test.fullScenario(conn, fixture);
        return;
    }

    // Otherwise run setUp.
    let cmdObj = test.command;
    if (typeof (test.setUp) === "function") {
        let setUpRes = test.setUp(conn, fixture);

        // For some commands (such as killSessions) the command requires information that is
        // created during the setUp portion (such as a session ID), so we need to create the
        // command in setUp. We set the command to an empty object in order to indicate that
        // the command created in setUp should be used instead.
        if (Object.keys(test.command).length === 0) {
            cmdObj = setUpRes;
        }
    }

    // Change cmdDb if necessary.
    if (test.isShardSvrOnly && isShardedCluster) {
        cmdDb = fixture.shard0.getDB(dbName);
    }
    if (test.isAdminCommand) {
        cmdDb = cmdDb.getSiblingDB("admin");
    }

    jsTestLog("Running command: " + tojson(cmdObj));
    if (test.expectFailure) {
        const expectedErrorCode = test.expectedErrorCode;
        assertCommandOrWriteFailed(
            cmdDb.runCommand(cmdObj), expectedErrorCode, () => tojson(cmdObj));
    } else {
        assert.commandWorked(cmdDb.runCommand(cmdObj), () => tojson(cmdObj));
    }

    // Run test teardown.
    if (typeof (test.teardown) === "function") {
        test.teardown(conn, fixture);
    }
};

let runTest = function(conn, adminDB, fixture) {
    let runDowngradingToUpgrading = false;
    if (FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        runDowngradingToUpgrading = true;
    }

    assert.commandFailed(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    jsTestLog("Running all commands in the downgradingToLastLTS FCV");
    // First check that the map contains all available commands.
    let commandsList = AllCommandsTest.checkCommandCoverage(conn, allCommands);
    if (isMongos(adminDB)) {
        let shardCommandsList =
            AllCommandsTest.checkCommandCoverage(fixture.shard0.rs.getPrimary(), allCommands);
        commandsList = new Set(commandsList.concat(shardCommandsList));
    }

    for (const command of commandsList) {
        const test = allCommands[command];

        // Coverage already guaranteed above, but check again just in case.
        assert(test, "Coverage failure: must explicitly define a test for " + command);

        if (test.skip !== undefined || test.skip === commandIsDisabledOnLastLTS) {
            jsTestLog("Skipping " + command + ": " + test.skip);
            continue;
        }

        // Run all commands.
        runAllCommands(command, test, conn, fixture);
    }

    if (runDowngradingToUpgrading) {
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

        jsTestLog("Running all commands after upgrading back to the latest FCV");
        commandsList = AllCommandsTest.checkCommandCoverage(conn, allCommands);
        if (isMongos(adminDB)) {
            let shardCommandsList =
                AllCommandsTest.checkCommandCoverage(fixture.shard0.rs.getPrimary(), allCommands);
            commandsList = new Set(commandsList.concat(shardCommandsList));
        }

        for (const command of commandsList) {
            const test = allCommands[command];

            // Coverage already guaranteed above, but check again just in case.
            assert(test, "Coverage failure: must explicitly define a test for " + command);

            if (test.skip !== undefined) {
                jsTestLog("Skipping " + command + ": " + test.skip);
                continue;
            }

            runAllCommands(command, test, conn, fixture);
        }
    }
};

let runStandaloneTest = function() {
    jsTestLog("Starting standalone test");
    const conn = MongoRunner.runMongod();
    const adminDB = conn.getDB("admin");

    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));

    runTest(conn, adminDB);
    MongoRunner.stopMongod(conn);
};

let runReplicaSetTest = function() {
    jsTestLog("Starting replica set test");
    const rst = new ReplSetTest(
        {name: name, nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryAdminDB = primary.getDB("admin");

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));

    runTest(primary, primaryAdminDB, rst);
    rst.stopSet();
};

let runShardedClusterTest = function() {
    jsTestLog("Starting sharded cluster test");
    const st = new ShardingTest({shards: 2, mongos: 1});
    const mongos = st.s;
    const mongosAdminDB = mongos.getDB("admin");
    const configPrimary = st.configRS.getPrimary();
    assert.commandWorked(
        configPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    runTest(mongos, mongosAdminDB, st);
    st.stop();
};

runStandaloneTest();
runReplicaSetTest();
runShardedClusterTest();
})();
