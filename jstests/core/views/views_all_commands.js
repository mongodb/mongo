// @tags: [
//   assumes_unsharded_collection,
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
//   requires_emptycapped,
//   requires_fastcount,
//   requires_getmore,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   uses_map_reduce_with_temp_collections,
//   # Tenant migrations don't support applyOps.
//   tenant_migration_incompatible,
//   # Explain of a resolved view must be executed by mongos.
//   directly_against_shardsvrs_incompatible,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
// ]

/*
 * Declaratively-defined tests for views for all database commands. This file contains a map of test
 * definitions as well as code to run them.
 *
 * The example test
 *
 *      {
 *          command: {insert: "view", documents: [{x: 1}]},
 *          expectFailure: true
 *      }
 *
 * invokes runCommand with `command` and expects it to fail with an error code specific to views.
 * A test can be an array of subtests as well.
 *
 * Each test or subtest takes the following options:
 *
 *  command
 *      The command object to pass to db.runCommand(). Each command can assume that there exists a
 *      view named "view", built on top of a collection named "collection". A command can also be a
 *      function that takes a db handle as argument and handles pass/failure checking internally.
 *
 *  isAdminCommand
 *      If true, will execute 'command' against the admin db.
 *
 *  skip
 *      A string that, if present, causes the test runner to skip running this command altogether.
 *      The value should be the reason why the test is being skipped. (There is a predefined
 *      selection of commonly-used reasons below.)
 *
 *  expectFailure
 *      If true, assert that the command fails. Otherwise, all commands are expected to succeed.
 *
 *  expectedErrorCode
 *      When 'expectFailure' is true, specifies the error code expected. Defaults to
 *      'CommandNotSupportedOnView' when not specified. Set to 'null' when expecting an error
 *      without an error code field.
 *
 *  setup
 *      A function that will be run before the command is executed. It takes a handle to the 'test'
 *      database as its single argument.
 *
 *  teardown
 *      A function that will be run after the command is executed. It takes a handle to the 'test'
 *      database as its single argument.
 *
 *  skipSharded
 *      If true, do not run this command on a mongos.
 *
 *  skipStandalone
 *      If true, do not run this command on a standalone mongod.
 */

(function() {
"use strict";

load('jstests/sharding/libs/last_lts_mongod_commands.js');

// Pre-written reasons for skipping a test.
const isAnInternalCommand = "internal command";
const isUnrelated = "is unrelated";

let viewsCommandTests = {
    _addShard: {skip: isAnInternalCommand},
    _cloneCatalogData: {skip: isAnInternalCommand},
    _cloneCollectionOptionsFromPrimaryShard: {skip: isAnInternalCommand},
    _configsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _configsvrAddShard: {skip: isAnInternalCommand},
    _configsvrAddShardToZone: {skip: isAnInternalCommand},
    _configsvrBalancerCollectionStatus: {skip: isAnInternalCommand},
    _configsvrBalancerStart: {skip: isAnInternalCommand},
    _configsvrBalancerStatus: {skip: isAnInternalCommand},
    _configsvrBalancerStop: {skip: isAnInternalCommand},
    _configsvrCleanupReshardCollection: {skip: isAnInternalCommand},
    _configsvrCollMod: {skip: isAnInternalCommand},
    _configsvrClearJumboFlag: {skip: isAnInternalCommand},
    _configsvrCommitChunksMerge: {skip: isAnInternalCommand},
    _configsvrCommitChunkMigration: {skip: isAnInternalCommand},
    _configsvrCommitChunkSplit: {skip: isAnInternalCommand},
    _configsvrCommitIndex: {skip: isAnInternalCommand},
    _configsvrCommitMovePrimary:
        {skip: isAnInternalCommand},  // Can be removed once 6.0 is last LTS
    _configsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _configsvrConfigureAutoSplit: {
        skip: isAnInternalCommand
    },  // TODO SERVER-62374: remove this once 5.3 becomes last continuos release
    _configsvrConfigureCollectionBalancing: {skip: isAnInternalCommand},
    _configsvrCreateDatabase: {skip: isAnInternalCommand},
    _configsvrDropIndexCatalogEntry: {skip: isAnInternalCommand},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isAnInternalCommand},
    _configsvrMoveChunk: {skip: isAnInternalCommand},  // Can be removed once 6.0 is last LTS
    _configsvrMovePrimary: {skip: isAnInternalCommand},
    _configsvrMoveRange: {skip: isAnInternalCommand},
    _configsvrRefineCollectionShardKey: {skip: isAnInternalCommand},
    _configsvrRenameCollection: {skip: isAnInternalCommand},
    _configsvrRenameCollectionMetadata: {skip: isAnInternalCommand},
    _configsvrRemoveChunks: {skip: isAnInternalCommand},
    _configsvrRemoveShard: {skip: isAnInternalCommand},
    _configsvrRemoveShardFromZone: {skip: isAnInternalCommand},
    _configsvrRemoveTags: {skip: isAnInternalCommand},
    _configsvrRepairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    _configsvrReshardCollection: {skip: isAnInternalCommand},
    _configsvrRunRestore: {skip: isAnInternalCommand},
    _configsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _configsvrSetClusterParameter: {skip: isAnInternalCommand},
    _configsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _configsvrUpdateZoneKeyRange: {skip: isAnInternalCommand},
    _flushDatabaseCacheUpdates: {skip: isUnrelated},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: isUnrelated},
    _flushReshardingStateChange: {skip: isUnrelated},
    _flushRoutingTableCacheUpdates: {skip: isUnrelated},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: isUnrelated},
    _getAuditConfigGeneration: {skip: isUnrelated},
    _getNextSessionMods: {skip: isAnInternalCommand},
    _getUserCacheGeneration: {skip: isAnInternalCommand},
    _hashBSONElement: {skip: isAnInternalCommand},
    _isSelf: {skip: isAnInternalCommand},
    _killOperations: {skip: isUnrelated},
    _mergeAuthzCollections: {skip: isAnInternalCommand},
    _migrateClone: {skip: isAnInternalCommand},
    _movePrimary: {skip: isAnInternalCommand},
    _recvChunkAbort: {skip: isAnInternalCommand},
    _recvChunkCommit: {skip: isAnInternalCommand},
    _recvChunkReleaseCritSec: {skip: isAnInternalCommand},
    _recvChunkStart: {skip: isAnInternalCommand},
    _recvChunkStatus: {skip: isAnInternalCommand},
    _shardsvrAbortReshardCollection: {skip: isAnInternalCommand},
    _shardsvrCloneCatalogData: {skip: isAnInternalCommand},
    _shardsvrCompactStructuredEncryptionData: {skip: isAnInternalCommand},
    _shardsvrDropCollection: {skip: isAnInternalCommand},
    _shardsvrDropCollectionIfUUIDNotMatching: {skip: isUnrelated},
    _shardsvrDropCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: isAnInternalCommand},
    _shardsvrDropIndexes: {skip: isAnInternalCommand},
    _shardsvrInsertGlobalIndexKey: {skip: isAnInternalCommand},
    _shardsvrCleanupReshardCollection: {skip: isAnInternalCommand},
    _shardsvrRegisterIndex: {skip: isAnInternalCommand},
    _shardsvrCommitIndexParticipant: {skip: isAnInternalCommand},
    _shardsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _shardsvrCreateCollection: {skip: isAnInternalCommand},
    _shardsvrCreateCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrCreateGlobalIndex: {skip: isAnInternalCommand},
    _shardsvrDropDatabase: {skip: isAnInternalCommand},
    _shardsvrDropDatabaseParticipant: {skip: isAnInternalCommand},
    _shardsvrGetStatsForBalancing: {skip: isAnInternalCommand},
    _shardsvrJoinMigrations: {skip: isAnInternalCommand},
    _shardsvrMovePrimary: {skip: isAnInternalCommand},
    _shardsvrMoveRange: {
        command: {_shardsvrMoveRange: "test.view"},
        skipStandalone: true,
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
    },
    _shardsvrRefineCollectionShardKey: {skip: isAnInternalCommand},
    _shardsvrRenameCollection: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipant: {skip: isAnInternalCommand},
    _shardsvrRenameCollectionParticipantUnblock: {skip: isAnInternalCommand},
    _shardsvrReshardCollection: {skip: isAnInternalCommand},
    _shardsvrReshardingOperationTime: {skip: isAnInternalCommand},
    _shardsvrSetAllowMigrations: {skip: isAnInternalCommand},
    _shardsvrSetClusterParameter: {skip: isAnInternalCommand},
    _shardsvrSetUserWriteBlockMode: {skip: isAnInternalCommand},
    _shardsvrCollMod: {skip: isAnInternalCommand},
    _shardsvrCollModParticipant: {skip: isAnInternalCommand},
    _shardsvrParticipantBlock: {skip: isAnInternalCommand},
    _shardsvrUnregisterIndex: {skip: isAnInternalCommand},
    _transferMods: {skip: isAnInternalCommand},
    _vectorClockPersist: {skip: isAnInternalCommand},
    abortReshardCollection: {skip: isUnrelated},
    abortTransaction: {skip: isUnrelated},
    addShard: {skip: isUnrelated},
    addShardToZone: {skip: isUnrelated},
    aggregate: {command: {aggregate: "view", pipeline: [{$match: {}}], cursor: {}}},
    analyze: {skip: isUnrelated},
    analyzeShardKey: {
        command: {analyzeShardKey: "test.view", key: {skey: 1}},
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true,
    },
    appendOplogNote: {skip: isUnrelated},
    applyOps: {
        command: {applyOps: [{op: "i", o: {_id: 1}, ns: "test.view"}]},
        expectFailure: true,
        skipSharded: true,
    },
    authenticate: {skip: isUnrelated},
    autoSplitVector: {
        command: {
            splitVector: "test.view",
            keyPattern: {x: 1},
            maxChunkSize: 1,
        },
        expectFailure: true,
    },
    balancerCollectionStatus: {
        command: {balancerCollectionStatus: "test.view"},
        setup: function(conn) {
            assert.commandWorked(conn.adminCommand({enableSharding: "test"}));
        },
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
    },
    balancerStart: {skip: isUnrelated},
    balancerStatus: {skip: isUnrelated},
    balancerStop: {skip: isUnrelated},
    buildInfo: {skip: isUnrelated},
    captrunc: {
        command: {captrunc: "view", n: 2, inc: false},
        expectFailure: true,
    },
    checkShardingIndex: {skip: isUnrelated},
    cleanupOrphaned: {
        skip: "Tested in views/views_sharded.js",
    },
    cleanupReshardCollection: {skip: isUnrelated},
    clearJumboFlag: {
        command: {clearJumboFlag: "test.view"},
        skipStandalone: true,
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
    },
    clearLog: {skip: isUnrelated},
    cloneCollectionAsCapped: {
        command: {cloneCollectionAsCapped: "view", toCollection: "testcapped", size: 10240},
        expectFailure: true,
    },
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {command: {collMod: "view", viewOn: "other", pipeline: []}},
    collStats: {skip: "Tested in views/views_coll_stats.js"},
    commitReshardCollection: {skip: isUnrelated},
    commitTransaction: {skip: isUnrelated},
    compact: {command: {compact: "view", force: true}, expectFailure: true, skipSharded: true},
    compactStructuredEncryptionData: {skip: isUnrelated},
    configureFailPoint: {skip: isUnrelated},
    configureCollectionAutoSplitter: {
        skip: isUnrelated
    },  // TODO SERVER-62374: remove this once 5.3 becomes last continuos release
    configureCollectionBalancing: {skip: isUnrelated},
    configureQueryAnalyzer: {
        command: {configureQueryAnalyzer: "test.view", mode: "full", sampleRate: 1},
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true,
    },
    connPoolStats: {skip: isUnrelated},
    connPoolSync: {skip: isUnrelated},
    connectionStatus: {skip: isUnrelated},
    convertToCapped: {command: {convertToCapped: "view", size: 12345}, expectFailure: true},
    coordinateCommitTransaction: {skip: isUnrelated},
    count: {command: {count: "view"}},
    cpuload: {skip: isAnInternalCommand},
    create: {skip: "tested in views/views_creation.js"},
    createIndexes: {
        command: {createIndexes: "view", indexes: [{key: {x: 1}, name: "x_1"}]},
        expectFailure: true,
    },
    createRole: {
        command: {createRole: "testrole", privileges: [], roles: []},
        setup: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
        }
    },
    createUser: {
        command: {createUser: "testuser", pwd: "testpass", roles: []},
        setup: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllUsersFromDatabase: 1}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllUsersFromDatabase: 1}));
        }
    },
    currentOp: {skip: isUnrelated},
    dataSize: {
        command: {dataSize: "test.view"},
        expectFailure: true,
    },
    dbCheck: {command: {dbCheck: "view"}, expectFailure: true},
    dbHash: {
        command: function(conn) {
            let getHash = function() {
                let cmd = {dbHash: 1};
                let res = conn.runCommand(cmd);
                assert.commandWorked(res, tojson(cmd));
                return res.collections["system.views"];
            };
            // The checksum below should change if we change the views, but not otherwise.
            let hash1 = getHash();
            assert.commandWorked(conn.runCommand({create: "view2", viewOn: "view"}),
                                 "could not create view 'view2' on 'view'");
            let hash2 = getHash();
            assert.neq(hash1, hash2, "expected hash to change after creating new view");
            assert.commandWorked(conn.runCommand({drop: "view2"}), "problem dropping view2");
            let hash3 = getHash();
            assert.eq(hash1, hash3, "hash should be the same again after removing 'view2'");
        }
    },
    dbStats: {command: {dbStats: 1}},
    delete: {command: {delete: "view", deletes: [{q: {x: 1}, limit: 1}]}, expectFailure: true},
    distinct: {command: {distinct: "view", key: "_id"}},
    donorAbortMigration: {skip: isUnrelated},
    // TODO : remove overrides once possible SERVER-61845
    donorAbortSplit: {skip: "has been removed from the server"},
    donorForgetMigration: {skip: isUnrelated},
    donorForgetSplit: {skip: "has been removed from the server"},
    donorStartMigration: {skip: isUnrelated},
    donorStartSplit: {skip: "has been removed from the server"},
    donorWaitForMigrationToCommit: {skip: isUnrelated},
    abortShardSplit: {skip: isUnrelated},
    commitShardSplit: {skip: isUnrelated},
    forgetShardSplit: {skip: isUnrelated},
    driverOIDTest: {skip: isUnrelated},
    drop: {command: {drop: "view"}},
    dropAllRolesFromDatabase: {skip: isUnrelated},
    dropAllUsersFromDatabase: {skip: isUnrelated},
    dropConnections: {skip: isUnrelated},
    dropDatabase: {command: {dropDatabase: 1}},
    dropIndexes: {command: {dropIndexes: "view", index: "a_1"}, expectFailure: true},
    dropRole: {
        command: {dropRole: "testrole"},
        setup: function(conn) {
            assert.commandWorked(
                conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
        }
    },
    dropUser: {skip: isUnrelated},
    echo: {skip: isUnrelated},
    emptycapped: {
        command: {emptycapped: "view"},
        expectFailure: true,
    },
    enableSharding: {skip: "Tested as part of shardCollection"},
    endSessions: {skip: isUnrelated},
    explain: {command: {explain: {count: "view"}}},
    features: {skip: isUnrelated},
    filemd5: {skip: isUnrelated},
    find: {skip: "tested in views/views_find.js & views/views_sharded.js"},
    findAndModify: {
        command: {findAndModify: "view", query: {a: 1}, update: {$set: {a: 2}}},
        expectFailure: true
    },
    flushRouterConfig: {skip: isUnrelated},
    fsync: {skip: isUnrelated},
    fsyncUnlock: {skip: isUnrelated},
    getAuditConfig: {skip: isUnrelated},
    getDatabaseVersion: {skip: isUnrelated},
    getChangeStreamState: {skip: isUnrelated},
    getClusterParameter: {skip: isUnrelated},
    getCmdLineOpts: {skip: isUnrelated},
    getDefaultRWConcern: {skip: isUnrelated},
    getDiagnosticData: {skip: isUnrelated},
    getFreeMonitoringStatus: {skip: isUnrelated},
    getLastError: {skip: isUnrelated},
    getLog: {skip: isUnrelated},
    getMore: {
        setup: function(conn) {
            assert.commandWorked(conn.collection.remove({}));
            assert.commandWorked(conn.collection.insert([{_id: 1}, {_id: 2}, {_id: 3}]));
        },
        command: function(conn) {
            function testGetMoreForCommand(cmd) {
                let res = conn.runCommand(cmd);
                assert.commandWorked(res, tojson(cmd));
                let cursor = res.cursor;
                assert.eq(
                    cursor.ns, "test.view", "expected view namespace in cursor: " + tojson(cursor));
                let expectedFirstBatch = [{_id: 1}, {_id: 2}];
                assert.eq(cursor.firstBatch, expectedFirstBatch, "returned wrong firstBatch");
                let getmoreCmd = {getMore: cursor.id, collection: "view"};
                res = conn.runCommand(getmoreCmd);

                assert.commandWorked(res, tojson(getmoreCmd));
                assert.eq("test.view",
                          res.cursor.ns,
                          "expected view namespace in cursor: " + tojson(res));
            }
            // find command.
            let findCmd = {find: "view", filter: {_id: {$gt: 0}}, batchSize: 2};
            testGetMoreForCommand(findCmd);

            // aggregate command.
            let aggCmd = {
                aggregate: "view",
                pipeline: [{$match: {_id: {$gt: 0}}}],
                cursor: {batchSize: 2}
            };
            testGetMoreForCommand(aggCmd);
        }
    },
    getParameter: {skip: isUnrelated},
    getShardMap: {skip: isUnrelated},
    getShardVersion: {
        command: {getShardVersion: "test.view"},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NoShardingEnabled,
        isAdminCommand: true,
        skipSharded: true,  // mongos is tested in views/views_sharded.js
    },
    getnonce: {skip: isUnrelated},
    godinsert: {skip: isAnInternalCommand},
    grantPrivilegesToRole: {skip: "tested in auth/commands_user_defined_roles.js"},
    grantRolesToRole: {skip: isUnrelated},
    grantRolesToUser: {skip: isUnrelated},
    handshake: {skip: isUnrelated},
    hello: {skip: isUnrelated},
    hostInfo: {skip: isUnrelated},
    httpClientRequest: {skip: isAnInternalCommand},
    exportCollection: {skip: isUnrelated},
    importCollection: {skip: isUnrelated},
    insert: {command: {insert: "view", documents: [{x: 1}]}, expectFailure: true},
    internalRenameIfOptionsAndIndexesMatch: {skip: isAnInternalCommand},
    invalidateUserCache: {skip: isUnrelated},
    isdbgrid: {skip: isUnrelated},
    isMaster: {skip: isUnrelated},
    killCursors: {
        setup: function(conn) {
            assert.commandWorked(conn.collection.remove({}));
            assert.commandWorked(conn.collection.insert([{_id: 1}, {_id: 2}, {_id: 3}]));
        },
        command: function(conn) {
            // First get and check a partial result for an aggregate command.
            let aggCmd = {aggregate: "view", pipeline: [{$sort: {_id: 1}}], cursor: {batchSize: 2}};
            let res = conn.runCommand(aggCmd);
            assert.commandWorked(res, tojson(aggCmd));
            let cursor = res.cursor;
            assert.eq(
                cursor.ns, "test.view", "expected view namespace in cursor: " + tojson(cursor));
            let expectedFirstBatch = [{_id: 1}, {_id: 2}];
            assert.eq(cursor.firstBatch, expectedFirstBatch, "aggregate returned wrong firstBatch");

            // Then check correct execution of the killCursors command.
            let killCursorsCmd = {killCursors: "view", cursors: [cursor.id]};
            res = conn.runCommand(killCursorsCmd);
            assert.commandWorked(res, tojson(killCursorsCmd));
            let expectedRes = {
                cursorsKilled: [cursor.id],
                cursorsNotFound: [],
                cursorsAlive: [],
                cursorsUnknown: [],
                ok: 1
            };
            delete res.operationTime;
            delete res.$clusterTime;
            assert.eq(expectedRes, res, "unexpected result for: " + tojson(killCursorsCmd));
        }
    },
    killOp: {skip: isUnrelated},
    killSessions: {skip: isUnrelated},
    killAllSessions: {skip: isUnrelated},
    killAllSessionsByPattern: {skip: isUnrelated},
    listCollections: {skip: "tested in views/views_creation.js"},
    listCommands: {skip: isUnrelated},
    listDatabases: {skip: isUnrelated},
    listDatabasesForAllTenants: {skip: isUnrelated},
    listIndexes: {command: {listIndexes: "view"}, expectFailure: true},
    listShards: {skip: isUnrelated},
    lockInfo: {skip: isUnrelated},
    logApplicationMessage: {skip: isUnrelated},
    logMessage: {skip: isUnrelated},
    logRotate: {skip: isUnrelated},
    logout: {skip: isUnrelated},
    makeSnapshot: {skip: isAnInternalCommand},
    mapReduce: {
        command:
            {mapReduce: "view", map: function() {}, reduce: function(key, vals) {}, out: "out"},
        expectFailure: true
    },
    mergeChunks: {
        command: {mergeChunks: "test.view", bounds: [{x: 0}, {x: 10}]},
        skipStandalone: true,
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
    },
    moveChunk: {
        command: {moveChunk: "test.view", find: {}, to: "a"},
        skipStandalone: true,
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
    },
    movePrimary: {skip: "Tested in sharding/movePrimary1.js"},
    moveRange: {skip: isUnrelated},
    multicast: {skip: isUnrelated},
    netstat: {skip: isAnInternalCommand},
    pinHistoryReplicated: {skip: isAnInternalCommand},
    ping: {command: {ping: 1}},
    planCacheClear: {command: {planCacheClear: "view"}, expectFailure: true},
    planCacheClearFilters: {command: {planCacheClearFilters: "view"}, expectFailure: true},
    planCacheListFilters: {command: {planCacheListFilters: "view"}, expectFailure: true},
    planCacheSetFilter: {command: {planCacheSetFilter: "view"}, expectFailure: true},
    prepareTransaction: {skip: isUnrelated},
    profile: {skip: isUnrelated},
    refineCollectionShardKey: {skip: isUnrelated},
    refreshLogicalSessionCacheNow: {skip: isAnInternalCommand},
    reapLogicalSessionCacheNow: {skip: isAnInternalCommand},
    recipientForgetMigration: {skip: isUnrelated},
    recipientSyncData: {skip: isUnrelated},
    recipientVoteImportedFiles: {skip: isAnInternalCommand},
    refreshSessions: {skip: isUnrelated},
    reIndex: {
        command: {reIndex: "view"},
        expectFailure: true,
        expectedErrorCode: [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView],
    },
    removeShard: {skip: isUnrelated},
    removeShardFromZone: {skip: isUnrelated},
    renameCollection: [
        {
            isAdminCommand: true,
            command: {renameCollection: "test.view", to: "test.otherview"},
            expectFailure: true,
            skipSharded: true,
        },
        {
            isAdminCommand: true,
            command: {renameCollection: "test.collection", to: "test.view"},
            expectFailure: true,
            expectedErrorCode: ErrorCodes.NamespaceExists,
            skipSharded: true,
        }
    ],
    repairDatabase: {skip: isUnrelated},
    repairShardedCollectionChunksHistory: {
        command: {repairShardedCollectionChunksHistory: "test.view"},
        skipStandalone: true,
        isAdminCommand: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotFound,
    },
    replSetAbortPrimaryCatchUp: {skip: isUnrelated},
    replSetFreeze: {skip: isUnrelated},
    replSetGetConfig: {skip: isUnrelated},
    replSetGetRBID: {skip: isUnrelated},
    replSetGetStatus: {skip: isUnrelated},
    replSetHeartbeat: {skip: isUnrelated},
    replSetInitiate: {skip: isUnrelated},
    replSetMaintenance: {skip: isUnrelated},
    replSetReconfig: {skip: isUnrelated},
    replSetRequestVotes: {skip: isUnrelated},
    replSetStepDown: {skip: isUnrelated},
    replSetStepUp: {skip: isUnrelated},
    replSetSyncFrom: {skip: isUnrelated},
    replSetTest: {skip: isUnrelated},
    replSetTestEgress: {skip: isUnrelated},
    replSetUpdatePosition: {skip: isUnrelated},
    replSetResizeOplog: {skip: isUnrelated},
    reshardCollection: {
        command: {reshardCollection: "test.view", key: {_id: 1}},
        setup: function(conn) {
            assert.commandWorked(conn.adminCommand({enableSharding: "test"}));
        },
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true,
    },
    revokePrivilegesFromRole: {
        command: {
            revokePrivilegesFromRole: "testrole",
            privileges: [{resource: {db: "test", collection: "view"}, actions: ["find"]}]
        },
        setup: function(conn) {
            assert.commandWorked(
                conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
        }
    },
    revokeRolesFromRole: {skip: isUnrelated},
    revokeRolesFromUser: {skip: isUnrelated},
    setAllowMigrations: {
        command: {setAllowMigrations: "test.view", allowMigrations: false},
        setup: function(conn) {
            assert.commandWorked(conn.adminCommand({enableSharding: "test"}));
        },
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true
    },
    rolesInfo: {skip: isUnrelated},
    rotateCertificates: {skip: isUnrelated},
    saslContinue: {skip: isUnrelated},
    saslStart: {skip: isUnrelated},
    sbe: {skip: isAnInternalCommand},
    serverStatus: {command: {serverStatus: 1}, skip: isUnrelated},
    setIndexCommitQuorum: {skip: isUnrelated},
    setAuditConfig: {skip: isUnrelated},
    setCommittedSnapshot: {skip: isAnInternalCommand},
    setDefaultRWConcern: {skip: isUnrelated},
    setFeatureCompatibilityVersion: {skip: isUnrelated},
    setFreeMonitoring: {skip: isUnrelated},
    setParameter: {skip: isUnrelated},
    setShardVersion: {skip: isUnrelated},
    setChangeStreamState: {skip: isUnrelated},
    setClusterParameter: {skip: isUnrelated},
    setUserWriteBlockMode: {skip: isUnrelated},
    shardCollection: {
        command: {shardCollection: "test.view", key: {_id: 1}},
        setup: function(conn) {
            assert.commandWorked(conn.adminCommand({enableSharding: "test"}));
        },
        skipStandalone: true,
        expectFailure: true,
        isAdminCommand: true,
    },
    shardingState: {skip: isUnrelated},
    shutdown: {skip: isUnrelated},
    sleep: {skip: isUnrelated},
    split: {
        command: {split: "test.view", find: {_id: 1}},
        skipStandalone: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NamespaceNotSharded,
        isAdminCommand: true,
    },
    splitChunk: {
        command: {
            splitChunk: "test.view",
            from: "shard0000",
            min: {x: MinKey},
            max: {x: 0},
            keyPattern: {x: 1},
            splitKeys: [{x: -2}, {x: -1}],
            shardVersion: {t: Timestamp(1, 2), e: ObjectId(), v: Timestamp(1, 1)}
        },
        skipSharded: true,
        expectFailure: true,
        expectedErrorCode: ErrorCodes.NoShardingEnabled,
        isAdminCommand: true,
    },
    splitVector: {
        command: {
            splitVector: "test.view",
            keyPattern: {x: 1},
            maxChunkSize: 1,
        },
        expectFailure: true,
    },
    stageDebug: {skip: isAnInternalCommand},
    startRecordingTraffic: {skip: isUnrelated},
    startSession: {skip: isAnInternalCommand},
    stopRecordingTraffic: {skip: isUnrelated},
    testDeprecation: {skip: isAnInternalCommand},
    testDeprecationInVersion2: {skip: isAnInternalCommand},
    testInternalTransactions: {skip: isAnInternalCommand},
    testRemoval: {skip: isAnInternalCommand},
    testReshardCloneCollection: {skip: isAnInternalCommand},
    testVersion2: {skip: isAnInternalCommand},
    testVersions1And2: {skip: isAnInternalCommand},
    top: {skip: "tested in views/views_stats.js"},
    update: {command: {update: "view", updates: [{q: {x: 1}, u: {x: 2}}]}, expectFailure: true},
    updateRole: {
        command: {
            updateRole: "testrole",
            privileges: [{resource: {db: "test", collection: "view"}, actions: ["find"]}]
        },
        setup: function(conn) {
            assert.commandWorked(
                conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
        }
    },
    updateUser: {skip: isUnrelated},
    updateZoneKeyRange: {skip: isUnrelated},
    usersInfo: {skip: isUnrelated},
    validate: {command: {validate: "view"}, expectFailure: true},
    validateDBMetadata:
        {command: {validateDBMetadata: 1, apiParameters: {version: "1", strict: true}}},
    waitForOngoingChunkSplits: {skip: isUnrelated},
    voteCommitImportCollection: {skip: isUnrelated},
    voteCommitIndexBuild: {skip: isUnrelated},
    voteCommitTransaction: {skip: isUnrelated},
    voteAbortTransaction: {skip: isUnrelated},
    waitForFailPoint: {skip: isUnrelated},
    whatsmyuri: {skip: isUnrelated},
    whatsmysni: {skip: isUnrelated}
};

commandsRemovedFromMongodSinceLastLTS.forEach(function(cmd) {
    viewsCommandTests[cmd] = {skip: "must define test coverage for backwards compatibility"};
});

/**
 * Helper function for failing commands or writes that checks the result 'res' of either.
 * If 'code' is null we only check for failure, otherwise we confirm error code matches as
 * well. On assert 'msg' is printed.
 */
let assertCommandOrWriteFailed = function(res, code, msg) {
    if (res.writeErrors !== undefined)
        assert.neq(0, res.writeErrors.length, msg);
    else if (res.code !== null)
        assert.commandFailedWithCode(res, code, msg);
    else
        assert.commandFailed(res, msg);
};

// Are we on a mongos?
var hello = db.runCommand("hello");
assert.commandWorked(hello);
var isMongos = (hello.msg === "isdbgrid");

// Obtain a list of all commands.
let res = db.runCommand({listCommands: 1});
assert.commandWorked(res);

let commands = Object.keys(res.commands);
for (let command of commands) {
    let test = viewsCommandTests[command];
    assert(test !== undefined,
           "Coverage failure: must explicitly define a views test for " + command);

    if (!(test instanceof Array))
        test = [test];
    let subtest_nr = 0;
    for (let subtest of test) {
        // Tests can be explicitly skipped. Print the name of the skipped test, as well as
        // the reason why.
        if (subtest.skip !== undefined) {
            print("Skipping " + command + ": " + subtest.skip);
            continue;
        }

        let dbHandle = db.getSiblingDB("test");
        let commandHandle = dbHandle;

        // Skip tests depending on sharding configuration.
        if (subtest.skipSharded && isMongos) {
            print("Skipping " + command + ": not applicable to mongoS");
            continue;
        }

        if (subtest.skipStandalone && !isMongos) {
            print("Skipping " + command + ": not applicable to mongoD");
            continue;
        }

        // Perform test setup, and call any additional setup callbacks provided by the test.
        // All tests assume that there exists a view named 'view' that is backed by
        // 'collection'.
        assert.commandWorked(dbHandle.dropDatabase());
        assert.commandWorked(dbHandle.runCommand({create: "view", viewOn: "collection"}));
        assert.commandWorked(dbHandle.collection.insert({x: 1}));
        if (subtest.setup !== undefined)
            subtest.setup(dbHandle);

        // Execute the command. Print the command name for the first subtest, as otherwise
        // it may be hard to figure out what command caused a failure.
        if (!subtest_nr++)
            print("Testing " + command);

        if (subtest.isAdminCommand)
            commandHandle = db.getSiblingDB("admin");

        if (subtest.expectFailure) {
            let expectedErrorCode = subtest.expectedErrorCode;
            if (expectedErrorCode === undefined)
                expectedErrorCode = ErrorCodes.CommandNotSupportedOnView;

            assertCommandOrWriteFailed(commandHandle.runCommand(subtest.command),
                                       expectedErrorCode,
                                       tojson(subtest.command));
        } else if (subtest.command instanceof Function)
            subtest.command(commandHandle);
        else
            assert.commandWorked(commandHandle.runCommand(subtest.command),
                                 tojson(subtest.command));

        if (subtest.teardown !== undefined)
            subtest.teardown(dbHandle);
    }
}
}());
