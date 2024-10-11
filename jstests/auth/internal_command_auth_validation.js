// This test ensures that internal commands only run with proper authorization and fail without
// proper authorization.
// @tags: [requires_profiling]

import {testOnlyCommands} from "jstests/auth/test_only_commands_list.js";
import {AllCommandsTest} from "jstests/libs/all_commands_test.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;
// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

const adminDbName = "admin";
const shard0name = "shard0000";
const dbName = "admin";
const collName = dbName + ".coll";
const ns = dbName + ".ns";
const dbPath = MongoRunner.toRealDir("$dataDir/commands_built_in_roles_sharded/");
const migrationOperationId = UUID();

const testOnlyCommandsSet = new Set(testOnlyCommands);
const sysUser = {
    user: "admin",
    pwd: "password",
    roles: ["__system"]
};
const noroleUser = {
    user: "testuser",
    pwd: "password",
    roles: []
};
/**
 * internalCommandsMap contains tests for each internal command. For each command name there is
 * a test object. Each test object inside the map has the following fields.
 *
 *  testname: The name of the command to run.
 *  command: This includes the command details to run using runCommand.
 *  precommand: This function is run before the sysUser runs the command (to be tested). This
 * creates preconditions so that the command is not blocked.
 *  postcommand: This function is run after the sysUser runs the command (to be tested)
 * successfully. This ensures other commands will not be blocked.
 *  skip: Some commands are skipped.
 */
const internalCommandsMap = {
    _clusterQueryWithoutShardKey: {
        testname: "_clusterQueryWithoutShardKey",
        command: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: "foo",
                updates: [
                    {q: {x: 1}, u: {$set: {a: 90}, upsert: false}},
                ]
            },
        },
    },
    _addShard: {
        testname: "_addShard",
        command: {
            _addShard: 1,
            shardIdentity: {
                shardName: shard0name,
                clusterId: ObjectId('5b2031806195dffd744258ee'),
                configsvrConnectionString: "foobarbaz/host:20022,host:20023,host:20024"
            }
        },
    },
    _clusterWriteWithoutShardKey: {
        testname: "_clusterWriteWithoutShardKey",
        command: {_clusterWriteWithoutShardKey: 1, writeCmd: {}, shardId: "", targetDocId: {}},
    },
    _configsvrAbortReshardCollection: {
        testname: "_configsvrAbortReshardCollection",
        command: {_configsvrAbortReshardCollection: "test.x"},
    },
    _configsvrAddShard: {
        testname: "_configsvrAddShard",
        command: {_configsvrAddShard: "x"},
    },
    _configsvrAddShardToZone: {
        testname: "_configsvrAddShardToZone",
        command: {_configsvrAddShardToZone: shard0name, zone: 'z'},
    },

    _configsvrBalancerStart: {
        testname: "_configsvrBalancerStart",
        command: {_configsvrBalancerStart: 1},

    },
    _configsvrBalancerStatus: {
        testname: "_configsvrBalancerStatus",
        command: {_configsvrBalancerStatus: 1},
    },
    _configsvrBalancerStop: {
        testname: "_configsvrBalancerStop",
        command: {_configsvrBalancerStop: 1},
    },
    _configsvrCommitChunkMigration: {
        testname: "_configsvrCommitChunkMigration",
        command: {
            _configsvrCommitChunkMigration: "db.fooHashed",
            fromShard: "move_chunk_basic-rs0",
            toShard: "move_chunk_basic-rs1",
            migratedChunk: {
                lastmod: {
                    e: new ObjectId('62b052ac7f5653479a67a54f'),
                    t: new Timestamp(1655722668, 22),
                    v: new Timestamp(1, 0)
                },
                min: {_id: MinKey},
                max: {_id: 611686018427387902}
            },
            fromShardCollectionVersion: {
                e: new ObjectId('62b052ac7f5653479a67a54f'),
                t: new Timestamp(1655722668, 22),
                v: new Timestamp(1, 3)
            },
            validAfter: new Timestamp(1655722670, 6)
        },
    },
    _configsvrBalancerCollectionStatus: {
        testname: "_configsvrBalancerCollectionStatus",
        command: {
            _configsvrBalancerCollectionStatus: "x.y",
        },
    },
    _configsvrCleanupReshardCollection: {
        testname: "_configsvrCleanupReshardCollection",
        command: {_configsvrCleanupReshardCollection: "test.x"},
    },
    _configsvrClearJumboFlag: {
        testname: "_configsvrClearJumboFlag",
        command:
            {_configsvrClearJumboFlag: "x.y", epoch: ObjectId(), minKey: {x: 0}, maxKey: {x: 10}},
    },
    _configsvrCommitChunksMerge: {
        testname: "_configsvrCommitChunksMerge",
        command: {
            _configsvrCommitChunksMerge: "x.y",
            shard: shard0name,
            collUUID: {uuid: UUID()},
            chunkRange: {min: {a: 1}, max: {a: 10}}
        },

    },
    _configsvrCommitChunkSplit: {
        testname: "_configsvrCommitChunkSplit",
        command: {_configsvrCommitChunkSplit: "x.y"},
    },
    _configsvrCommitIndex: {
        testname: "_configsvrCommitIndex",
        command: {
            _configsvrCommitIndex: "x.y",
            keyPattern: {x: 1},
            name: 'x_1',
            options: {},
            collectionUUID: UUID(),
            collectionIndexUUID: UUID(),
            lastmod: Timestamp(1, 0),
        },
    },
    _configsvrCommitRefineCollectionShardKey: {
        testname: "_configsvrCommitRefineCollectionShardKey",
        command: {
            _configsvrCommitRefineCollectionShardKey: "test.x",
            key: {aKey: 1},
            newEpoch: new ObjectId(),
            newTimestamp: Timestamp(),
            oldTimestamp: Timestamp()
        },
    },
    _configsvrCommitMergeAllChunksOnShard: {
        testname: "_configsvrCommitMergeAllChunksOnShard",
        command: {_configsvrCommitMergeAllChunksOnShard: "test.x", shard: shard0name},
    },
    _configsvrCommitReshardCollection: {
        testname: "_configsvrCommitReshardCollection",
        command: {_configsvrCommitReshardCollection: "test.x"},
    },
    _configsvrConfigureCollectionBalancing: {
        testname: "_configsvrConfigureCollectionBalancing",
        command: {_configsvrConfigureCollectionBalancing: "test.x"},
    },
    _configsvrCreateDatabase: {
        testname: "_configsvrCreateDatabase",
        command: {_configsvrCreateDatabase: "test.x", primaryShardId: ""},
    },
    _configsvrDropIndexCatalogEntry: {
        testname: "_configsvrDropIndexCatalogEntry",
        command: {
            _configsvrDropIndexCatalogEntry: "x.y",
            name: 'x_1',
            collectionUUID: UUID(),
            lastmod: Timestamp(1, 0),
        },
    },
    _configsvrCheckClusterMetadataConsistency: {
        testname: "_configsvrCheckClusterMetadataConsistency",
        command: {
            _configsvrCheckClusterMetadataConsistency: "x.y",
            cursor: {},
        },
    },
    _configsvrCheckMetadataConsistency: {
        testname: "_configsvrCheckMetadataConsistency",
        command: {
            _configsvrCheckMetadataConsistency: "x.y",
            cursor: {},
        },
    },
    _configsvrCollMod: {
        testname: "_configsvrCollMod",
        command: {
            _configsvrCollMod: "x.y",
            collModRequest: {},
        },
    },
    _configsvrCommitMovePrimary: {
        testname: "_configsvrCommitMovePrimary",
        command: {
            _configsvrCommitMovePrimary: "test",
            expectedDatabaseVersion: {
                uuid: new UUID(),
                timestamp: new Timestamp(1691525961, 12),
                lastMod: NumberInt(5),
            },
            to: shard0name
        },
    },
    _configsvrEnsureChunkVersionIsGreaterThan: {
        testname: "_configsvrEnsureChunkVersionIsGreaterThan",
        command: {
            _configsvrEnsureChunkVersionIsGreaterThan: "x.y",
            minKey: {},
            maxKey: {},
            version: {e: ObjectId("6657bdabfd296e9f62d2816c"), t: Timestamp(), v: Timestamp()},
            collectionUUID: UUID(),
            nss: ns,
        },
    },
    _configsvrGetHistoricalPlacement: {
        testname: "_configsvrGetHistoricalPlacement",
        command: {
            _configsvrGetHistoricalPlacement: "x.y",
            at: new Timestamp(1691525961, 12),
        },
    },
    _configsvrMoveRange: {
        testname: "_configsvrMoveRange",
        command: {
            _configsvrMoveRange: "x.y",
            toShard: shard0name,
        },
    },
    _configsvrRemoveChunks: {
        testname: "_configsvrRemoveChunks",
        command: {
            _configsvrRemoveChunks: 1,
            collectionUUID: UUID(),
        },
    },
    _configsvrRemoveShard: {
        testname: "_configsvrRemoveShard",
        command: {_configsvrRemoveShard: 1, removeShard: shard0name},
    },
    _configsvrRemoveShardFromZone: {
        testname: "_configsvrRemoveShardFromZone",
        command: {_configsvrRemoveShardFromZone: 1, removeShard: shard0name, zone: 'z'},
    },
    _configsvrRemoveTags: {
        testname: "_configsvrRemoveTags",
        command: {_configsvrRemoveTags: "test"},
    },
    _configsvrRepairShardedCollectionChunksHistory: {
        testname: "_configsvrRepairShardedCollectionChunksHistory",
        command: {_configsvrRepairShardedCollectionChunksHistory: ns},
    },
    _configsvrResetPlacementHistory: {
        testname: "_configsvrResetPlacementHistory",
        command: {_configsvrResetPlacementHistory: ns},
    },
    _configsvrReshardCollection: {
        testname: "_configsvrReshardCollection",
        command: {_configsvrReshardCollection: ns, key: {_id: 1}},
    },
    _configsvrRunRestore: {
        testname: "_configsvrRunRestore",
        command: {_configsvrRunRestore: 1},
    },
    _configsvrSetAllowMigrations: {
        testname: "_configsvrSetAllowMigrations",
        command: {
            _configsvrSetAllowMigrations: ns,
            allowMigrations: false,
            writeConcern: {w: "majority"}
        },
    },
    _configsvrSetClusterParameter: {
        testname: "_configsvrSetClusterParameter",
        command: {
            _configsvrSetClusterParameter: {},
        },
    },
    _configsvrSetUserWriteBlockMode: {
        testname: "_configsvrSetUserWriteBlockMode",
        command: {_configsvrSetUserWriteBlockMode: 1, global: true},
    },
    _configsvrTransitionFromDedicatedConfigServer: {
        testname: "_configsvrTransitionFromDedicatedConfigServer",
        command: {_configsvrTransitionFromDedicatedConfigServer: 1},
    },
    _configsvrTransitionToDedicatedConfigServer: {
        testname: "_configsvrTransitionToDedicatedConfigServer",
        command: {_configsvrTransitionToDedicatedConfigServer: 1},
    },
    _configsvrUpdateZoneKeyRange: {
        testname: "_configsvrUpdateZoneKeyRange",
        command: {_configsvrUpdateZoneKeyRange: 'test.foo', min: {x: 1}, max: {x: 5}, zone: 'z'},
    },
    _dropConnectionsToMongot: {
        testname: "_dropConnectionsToMongot",
        command: {_dropConnectionsToMongot: 1, hostAndPort: []},
    },
    _flushDatabaseCacheUpdates: {
        testname: "_flushDatabaseCacheUpdates",
        command: {_flushDatabaseCacheUpdates: 'test'},
    },
    _flushDatabaseCacheUpdatesWithWriteConcern: {
        testname: "_flushDatabaseCacheUpdatesWithWriteConcern",
        command: {_flushDatabaseCacheUpdatesWithWriteConcern: 'test', writeConcern: {w: 2}},
    },
    _flushReshardingStateChange: {
        testname: "_flushReshardingStateChange",
        command: {
            _flushReshardingStateChange: ns,
            reshardingUUID: UUID(),
        },
    },
    _flushRoutingTableCacheUpdates: {
        testname: "_flushRoutingTableCacheUpdates",
        command: {
            _flushRoutingTableCacheUpdates: ns,
        },
    },
    _flushRoutingTableCacheUpdatesWithWriteConcern: {
        testname: "_flushRoutingTableCacheUpdatesWithWriteConcern",
        command: {_flushRoutingTableCacheUpdatesWithWriteConcern: ns, writeConcern: {w: 2}},
    },
    _getAuditConfigGeneration: {
        testname: "_getAuditConfigGeneration",
        command: {_getAuditConfigGeneration: 1},
    },
    _getNextSessionMods: {
        testname: "_getNextSessionMods",
        command: {_getNextSessionMods: "a-b"},
    },
    _getUserCacheGeneration: {
        testname: "_getUserCacheGeneration",
        command: {_getUserCacheGeneration: 1},
    },
    _hashBSONElement: {
        testname: "_hashBSONElement",
        command: {_hashBSONElement: 0, seed: 1},
    },
    _isSelf: {
        skip: true,  // This command does not need '__system' role as it is currently used in atlas
                     // tools.
        testname: "_isSelf",
        command: {_isSelf: 1},
    },
    _killOperations: {
        testname: "_killOperations",
        command: {_killOperations: 1, operationKeys: [UUID()]},
    },
    _mergeAuthzCollections: {
        testname: "_mergeAuthzCollections",
        command: {
            _mergeAuthzCollections: 1,
            tempUsersCollection: 'admin.tempusers',
            tempRolesCollection: 'admin.temproles',
            db: "",
            drop: false
        },
    },
    _migrateClone: {
        testname: "_migrateClone",
        command: {_migrateClone: "test"},
    },
    _mongotConnPoolStats: {
        testname: "_mongotConnPoolStats",
        command: {_mongotConnPoolStats: 1},
    },
    _recvChunkAbort: {
        testname: "_recvChunkAbort",
        command: {_recvChunkAbort: 1},
    },
    _recvChunkCommit: {
        testname: "_recvChunkCommit",
        command: {_recvChunkCommit: 1},
    },
    _recvChunkReleaseCritSec: {
        testname: "_recvChunkReleaseCritSec",
        command: {_recvChunkReleaseCritSec: 1},
    },
    _recvChunkStart: {
        testname: "_recvChunkStart",
        command: {_recvChunkStart: ns},
    },
    _recvChunkStatus: {
        testname: "_recvChunkStatus",
        command: {_recvChunkStatus: ns},
    },
    _refreshQueryAnalyzerConfiguration: {
        testname: "_refreshQueryAnalyzerConfiguration",
        command:
            {_refreshQueryAnalyzerConfiguration: 1, name: "test", numQueriesExecutedPerSecond: 1},
    },
    _shardsvrAbortReshardCollection: {
        testname: "_shardsvrAbortReshardCollection",
        command: {_shardsvrAbortReshardCollection: UUID(), userCanceled: true},
    },
    _shardsvrBeginMigrationBlockingOperation: {
        testname: "_shardsvrBeginMigrationBlockingOperation",
        command: {_shardsvrBeginMigrationBlockingOperation: ns, operationId: migrationOperationId},
        postcommand: (db) => {
            assert.commandWorked(db.runCommand(
                {_shardsvrEndMigrationBlockingOperation: ns, operationId: migrationOperationId}));
        },
    },
    _shardsvrChangePrimary: {
        testname: "_shardsvrChangePrimary",
        command: {
            _shardsvrChangePrimary: "test",
            expectedDatabaseVersion: {
                uuid: new UUID(),
                timestamp: new Timestamp(1691525961, 12),
                lastMod: NumberInt(5),
            },
            to: shard0name
        },
    },
    _shardsvrCleanupStructuredEncryptionData: {
        testname: "_shardsvrCleanupStructuredEncryptionData",
        command: {_shardsvrCleanupStructuredEncryptionData: "test", cleanupTokens: {}},
    },
    _shardsvrCleanupReshardCollection: {
        testname: "_shardsvrCleanupReshardCollection",
        command: {_shardsvrCleanupReshardCollection: "test.x", reshardingUUID: UUID()},
    },
    _shardsvrCloneCatalogData: {
        testname: "_shardsvrCloneCatalogData",
        command: {_shardsvrCloneCatalogData: 'test', from: [], writeConcern: {w: "majority"}},
    },
    _shardsvrCompactStructuredEncryptionData: {
        testname: "_shardsvrCompactStructuredEncryptionData",
        command: {_shardsvrCompactStructuredEncryptionData: 'test', compactionTokens: {}},
    },
    _shardsvrConvertToCapped: {
        testname: "_shardsvrConvertToCapped",
        command: {_shardsvrConvertToCapped: 'test', size: 0},
    },
    _shardsvrRegisterIndex: {
        testname: "_shardsvrRegisterIndex",
        command: {
            _shardsvrRegisterIndex: ns,
            keyPattern: {x: 1},
            options: {},
            name: 'x_1',
            collectionUUID: UUID(),
            indexCollectionUUID: UUID(),
            lastmod: Timestamp(0, 0),
            writeConcern: {w: 'majority'}
        },
    },
    _shardsvrCommitIndexParticipant: {
        testname: "_shardsvrCommitIndexParticipant",
        command: {
            _shardsvrCommitIndexParticipant: "x.y",
            name: 'x_1',
            keyPattern: {x: 1},
            options: {},
            collectionUUID: UUID(),
            lastmod: Timestamp(1, 0),
        },
    },
    _shardsvrCommitReshardCollection: {
        testname: "_shardsvrCommitReshardCollection",
        command: {
            _shardsvrCommitReshardCollection: "x.y",
            reshardingUUID: UUID(),
        },
    },
    _shardsvrDropCollection: {
        testname: "_shardsvrDropCollection",
        command: {
            _shardsvrDropCollection: "x.y",
            collectionUUID: UUID(),
        },
    },
    _shardsvrConvertToCappedParticipant: {
        testname: "_shardsvrConvertToCappedParticipant",
        command: {
            _shardsvrConvertToCappedParticipant: "x.y",
            size: 0,
            targetUUID: UUID(),
        },
    },
    _shardsvrJoinDDLCoordinators: {
        testname: "_shardsvrJoinDDLCoordinators",
        command: {
            _shardsvrJoinDDLCoordinators: "x.y",
        },
    },
    _shardsvrCreateCollection: {
        testname: "_shardsvrCreateCollection",
        command: {
            _shardsvrCreateCollection: "x.y",
            collectionUUID: UUID(),
        },
    },
    _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {
        testname: "_shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern",
        command: {
            _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: "x.y",
            expectedCollectionUUID: UUID(),
        },
    },
    _shardsvrDropCollectionParticipant: {
        testname: "_shardsvrDropCollectionParticipant",
        command: {
            _shardsvrDropCollectionParticipant: "x.y",
        },
    },
    _shardsvrDropIndexCatalogEntryParticipant: {
        testname: "_shardsvrDropIndexCatalogEntryParticipant",
        command: {
            _shardsvrDropIndexCatalogEntryParticipant: "x.y",
            name: 'x_1',
            collectionUUID: UUID(),
            lastmod: Timestamp(1, 0),
        },
    },
    _shardsvrDropIndexes: {
        testname: "_shardsvrDropIndexes",
        command: {
            _shardsvrDropIndexes: "x.y",
            index: "*",
            collectionUUID: UUID(),
        },
    },
    _shardsvrCreateCollectionParticipant: {
        testname: "_shardsvrCreateCollectionParticipant",
        command: {
            _shardsvrCreateCollectionParticipant: "x.y",
            indexes: [],
            options: {},
            idIndex: {v: 2, key: {_id: 1}, name: "_id_"},
        },
    },
    _shardsvrCoordinateMultiUpdate: {
        testname: "_shardsvrCoordinateMultiUpdate",
        precommand: (precommanddb, user, pwd, coll) => {
            assert(precommanddb.auth(user, pwd));
            assert.commandWorked(coll.insert({x: 1, y: 1}));
            assert.commandWorked(coll.insert({x: 1, y: 2}));
            precommanddb.logout();
        },
        command: {
            _shardsvrCoordinateMultiUpdate: collName,
            uuid: UUID(),
            command: {update: collName, updates: [{q: {x: 1}, u: {$set: {y: 2}}, multi: true}]}
        },
    },
    _shardsvrEndMigrationBlockingOperation: {
        testname: "_shardsvrEndMigrationBlockingOperation",
        command: {
            _shardsvrEndMigrationBlockingOperation: "ns",
            operationId: migrationOperationId,
        },
    },
    _shardsvrGetStatsForBalancing: {
        testname: "_shardsvrGetStatsForBalancing",
        command: {_shardsvrGetStatsForBalancing: "ns", collections: [], scaleFactor: 1},
    },
    _shardsvrJoinMigrations: {
        testname: "_shardsvrJoinMigrations",
        command: {_shardsvrJoinMigrations: 1},
    },
    _shardsvrMergeAllChunksOnShard: {
        testname: "_shardsvrMergeAllChunksOnShard",
        command: {
            _shardsvrMergeAllChunksOnShard: "x.y",
            shard: shard0name,
            maxNumberOfChunksToMerge: NumberInt(2)
        },
    },
    _shardsvrMovePrimary: {
        testname: "_shardsvrMovePrimary",
        command: {
            _shardsvrMovePrimary: "test",
            expectedDatabaseVersion: {
                uuid: new UUID(),
                timestamp: new Timestamp(1691525961, 12),
                lastMod: NumberInt(5),
            },
            to: shard0name
        },
    },
    _shardsvrMovePrimaryEnterCriticalSection: {
        testname: "_shardsvrMovePrimaryEnterCriticalSection",
        command: {_shardsvrMovePrimaryEnterCriticalSection: "test", reason: {}},
    },
    _shardsvrMovePrimaryExitCriticalSection: {
        testname: "_shardsvrMovePrimaryExitCriticalSection",
        command: {_shardsvrMovePrimaryExitCriticalSection: "test", reason: {}},
    },
    _shardsvrMoveRange: {
        testname: "_shardsvrMoveRange",
        command: {
            _shardsvrMoveRange: "test.view",
            fromShard: shard0name,
            toShard: "shard0001",
            maxChunkSizeBytes: NumberInt(100)
        },
    },
    _shardsvrNotifyShardingEvent: {
        testname: "_shardsvrNotifyShardingEvent",
        command: {_shardsvrNotifyShardingEvent: "test", eventType: "databasesAdded", details: {}},
    },
    _shardsvrRenameCollection: {
        testname: "_shardsvrRenameCollection",
        command: {_shardsvrRenameCollection: "test.collection", to: "db.collection_renamed"},
    },
    _shardsvrRenameCollectionParticipant: {
        testname: "_shardsvrRenameCollectionParticipant",
        command: {
            _shardsvrRenameCollectionParticipant: "test.collection",
            to: "db.collection_renamed",
            sourceUUID: UUID()
        },
    },
    _shardsvrRenameCollectionParticipantUnblock: {
        testname: "_shardsvrRenameCollectionParticipantUnblock",
        command: {
            _shardsvrRenameCollectionParticipantUnblock: "test.collection",
            to: "db.collection_renamed",
            sourceUUID: UUID()
        },
    },
    _shardsvrRenameIndexMetadata: {
        testname: "_shardsvrRenameIndexMetadata",
        command: {
            _shardsvrRenameIndexMetadata: "test.collection",
            toNss: ns,
            indexVersion: {uuid: UUID(), version: Timestamp()},
        },
    },
    _shardsvrDropDatabase: {
        testname: "_shardsvrDropDatabase",
        command: {
            _shardsvrDropDatabase: 1,
        },
    },
    _shardsvrDropDatabaseParticipant: {
        testname: "_shardsvrDropDatabaseParticipant",
        command: {
            _shardsvrDropDatabaseParticipant: "test.x",
        },
    },
    _shardsvrReshardCollection: {
        testname: "_shardsvrReshardCollection",
        command: {
            _shardsvrReshardCollection: "test.x",
            phase: "unset",
            key: {},
        },
    },
    _shardsvrReshardingOperationTime: {
        testname: "_shardsvrReshardingOperationTime",
        command: {
            _shardsvrReshardingOperationTime: "test.x",
        },
    },
    _shardsvrRefineCollectionShardKey: {
        testname: "_shardsvrRefineCollectionShardKey",
        command: {_shardsvrRefineCollectionShardKey: "test.x", newShardKey: {}},
    },
    _shardsvrSetAllowMigrations: {
        testname: "_shardsvrSetAllowMigrations",
        command: {_shardsvrSetAllowMigrations: "db.collection", allowMigrations: true},
    },
    _shardsvrSetClusterParameter: {
        testname: "_shardsvrSetClusterParameter",
        command: {_shardsvrSetClusterParameter: {}, clusterParameterTime: Timestamp()},
    },
    _shardsvrSetUserWriteBlockMode: {
        testname: "_shardsvrSetUserWriteBlockMode",
        command: {
            _shardsvrSetUserWriteBlockMode: 1,
            global: true,
            phase: 'complete',
        },
    },
    _shardsvrValidateShardKeyCandidate: {
        testname: "_shardsvrValidateShardKeyCandidate",
        command: {_shardsvrValidateShardKeyCandidate: "x.y", key: {a: 1}},
    },
    _shardsvrCollModParticipant: {
        testname: "_shardsvrCollModParticipant",
        command: {
            _shardsvrCollModParticipant: "x.y",
            collModRequest: {},
        },
    },
    _shardsvrCollMod: {
        testname: "_shardsvrCollMod",
        command: {
            _shardsvrCollMod: "x.y",
        },
    },
    _shardsvrParticipantBlock: {
        testname: "_shardsvrParticipantBlock",
        command: {
            _shardsvrParticipantBlock: "x.y",
        },
    },
    _shardsvrUnregisterIndex: {
        testname: "_shardsvrUnregisterIndex",
        command: {
            _shardsvrUnregisterIndex: "x.y",
            name: 'x_1',
            collectionUUID: UUID(),
            lastmod: Timestamp(1, 0),
        },
    },
    _shardsvrUntrackUnsplittableCollection: {
        testname: "_shardsvrUntrackUnsplittableCollection",
        command: {_shardsvrUntrackUnsplittableCollection: "x.y", writeConcern: {w: 'majority'}},
    },
    _shardsvrCheckMetadataConsistency: {
        testname: "_shardsvrCheckMetadataConsistency",
        command: {
            _shardsvrCheckMetadataConsistency: "x.y",
        },
    },
    _shardsvrCheckMetadataConsistencyParticipant: {
        testname: "_shardsvrCheckMetadataConsistencyParticipant",
        command: {
            _shardsvrCheckMetadataConsistencyParticipant: "x.y",
            primaryShardId: shard0name,
        },
    },
    _transferMods: {
        testname: "_transferMods",
        command: {
            _transferMods: "x.y",
            sessionId: {id: UUID()},
        },
    },
};

/**
 * Parameters:
 *   db -- database object where the user is created.
 *   userName -- the username.
 *   roles -- roles for the user
 *
 * Returns:
 *   void after creating the user for the test case.
 */
function createUser(db, userName, roles) {
    assert(db.auth("admin", "password"));
    db.createUser({user: userName, pwd: "password", roles: roles});
    db.logout();
    return userName;
}
/**
 * runOneCommandAuthorizationTest runs authorization failure and success tests for a single command.
 * If the user with __system role can run the command without getting Unauthorized error AND if the
 * user with no roles can run the command and get an 'Unauthorized` error, then we consider the test
 * as passed. Otherwise, the command fails the authorization check.
 *
 *  Parameters:
 *     testObject -- testObject contains the test information for a single command.
 *     commandName -- command Name for the test.
 *     db -- db on which the command is going to run.
 *     secondDb -- the precommands are run on the second db.
 *     coll -- collection for the second db command.
 * Returns:
 *    result includes the pass/fail value and the command results for both system role
 * and no roles.
 */
function runOneCommandAuthorizationTest(testObject, commandName, db, secondDb, coll) {
    const cmdOK = {commandWorked: 1, commandFailed: 0};
    let result = {pass: true};
    if (testObject.precommand != undefined) {
        if (secondDb == undefined) {
            return result;
        }
        testObject.precommand(secondDb, sysUser.user, sysUser.pwd, coll);
    }
    assert(db.auth(noroleUser.user, noroleUser.pwd));
    result.noRoleRes = db.runCommand(testObject.command);
    db.logout();
    if (result.noRoleRes.ok === cmdOK.commandWorked ||
        result.noRoleRes.code !== ErrorCodes.Unauthorized) {
        result.pass = false;
        return result;
    }
    assert(db.auth(sysUser.user, sysUser.pwd));
    result.sysRes = db.runCommand(testObject.command);
    if (result.sysRes.ok === cmdOK.commandWorked && testObject.postcommand !== undefined) {
        testObject.postcommand(db);
    }
    db.logout();
    const sysRolePass = (result.sysRes.ok === cmdOK.commandWorked ||
                         result.sysRes.code !== ErrorCodes.Unauthorized);

    assert(sysRolePass);
    return result;
}

/**
 * Some commands may be skipped if they are test only commands. isSelf command is also skipped.
 * @param testObjectForCommand test object for the command.
 * @param commandName command Name.
 * @returns true if the command is a test only command or if the command needs to be skipped(
 *     currently only isSelf is skipped. It is used by atlas tools without __system role).
 */
function skipCommand(testObjectForCommand, commandName) {
    if (testOnlyCommandsSet.has(commandName) || testObjectForCommand.skip) {
        return true;
    }
    return false;
}
/**
 * Parameters:
 *   conn -- connection, either to standalone mongod,
 *      or to mongos in sharded cluster
 *   firstDb -- all commands are tested on the first db.
 *   secondDb -- the precommands are run on the second db.
 *   coll -- collection for the second db command.
 *
 * Returns:
 *   results of the test
 */
function runAuthorizationTestsOnAllInternalCommands(conn, firstDb, secondDb, coll) {
    let results = {};
    let fails = [];
    const availableCommandsList =
        AllCommandsTest.checkCommandCoverage(conn, internalCommandsMap, function(cmdName) {
            return !cmdName.startsWith('_') || testOnlyCommandsSet.hasOwnProperty(cmdName);
        });
    for (const commandName of availableCommandsList) {
        const test = internalCommandsMap[commandName];

        assert(test, "Coverage failure: must explicitly define a test for " + commandName);
        if (skipCommand(test, commandName)) {
            continue;
        }
        results[commandName] =
            runOneCommandAuthorizationTest(test, commandName, firstDb, secondDb, coll);
    }
    return results;
}

/**
 * Sets up the standalone test setup and runs authorization tests on all internal commands.
 */
function runStandaloneTest() {
    jsTestLog("Starting standalone test");

    // Setup standalone mongod, create a DB and create the admin
    const dbPath = MongoRunner.toRealDir("$dataDir/commands_built_in_roles_standalone/");
    mkdir(dbPath);
    const opts = {
        auth: "",
        setParameter: {
            trafficRecordingDirectory: dbPath,
            mongotHost: "localhost:27017",  // We have to set the mongotHost parameter for the
                                            // $search-relatead tests to pass configuration checks.
            syncdelay:
                0  // Disable checkpoints as this can cause some commands to fail transiently.
        }
    };

    const conn = MongoRunner.runMongod(opts);
    const adminDb = conn.getDB(adminDbName);
    adminDb.createUser({user: "admin", pwd: "password", roles: ["__system"]});
    createUser(adminDb, noroleUser.user, noroleUser.roles);

    const results = runAuthorizationTestsOnAllInternalCommands(conn, adminDb);

    MongoRunner.stopMongod(conn);
    return results;
}

/**
 * Sets up the sharded cluster test setup.
 */
function setupShardedClusterTest() {
    mkdir(dbPath);
    const opts = {
        auth: "",
        setParameter: {
            trafficRecordingDirectory: dbPath,
            mongotHost: "localhost:27017",  // We have to set the mongotHost parameter for the
                                            // $search-related tests to pass configuration checks.
            syncdelay:
                0  // Disable checkpoints as this can cause some commands to fail transiently.
        }
    };
    return opts;
}

/**
 * run mongos test.
 */
function runMongosTest(opts) {
    jsTestLog("Starting mongos test");
    const shardingTest = new ShardingTest({
        shards: 2,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        other: {
            shardOptions: opts,
            // We have to set the mongotHost parameter for the $search-related tests to pass
            // configuration checks.
            mongosOptions:
                {setParameter: {trafficRecordingDirectory: dbPath, mongotHost: "localhost:27017"}}
        }
    });
    const mongos = shardingTest.s0;
    const mongosAdminDB = mongos.getDB("admin");
    mongosAdminDB.createUser({user: "admin", pwd: "password", roles: ["__system"]});
    createUser(mongosAdminDB, noroleUser.user, noroleUser.roles);
    const results = runAuthorizationTestsOnAllInternalCommands(mongos, mongosAdminDB);
    shardingTest.stop();
    return results;
}
/**
 * run sharded server test.
 */
function runShardedServerTest(opts) {
    const shardingTest = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        useHostname: false,
        other: {
            shardOptions: opts,
            // We have to set the mongotHost parameter for the $search-related tests to pass
            // configuration checks.
            mongosOptions:
                {setParameter: {trafficRecordingDirectory: dbPath, mongotHost: "localhost:27017"}}
        }
    });
    const shardPrimary = shardingTest.rs0.getPrimary();
    const shardAdminDB = shardPrimary.getDB("admin");
    const mongosDB = shardingTest.s0.getDB("admin");
    const coll = mongosDB.getCollection(collName);
    mongosDB.createUser({user: "admin", pwd: "password", roles: ["__system"]});
    if (!TestData.configShard) {
        shardAdminDB.createUser({user: "admin", pwd: "password", roles: ["__system"]});
    }
    createUser(shardAdminDB, noroleUser.user, noroleUser.roles);
    const results =
        runAuthorizationTestsOnAllInternalCommands(shardPrimary, shardAdminDB, mongosDB, coll);
    shardingTest.stop();
    return results;
}
/**
 * run config server test.
 */
function runConfigServer(opts) {
    const shardingTest = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        keyFile: "jstests/libs/key1",
        other: {
            shardOptions: opts,
            // We have to set the mongotHost parameter for the $search-related tests to pass
            // configuration checks.
            mongosOptions:
                {setParameter: {trafficRecordingDirectory: dbPath, mongotHost: "localhost:27017"}}
        }
    });

    const configPrimary = shardingTest.configRS.getPrimary();
    const configAdminDB = configPrimary.getDB("admin");
    configAdminDB.createUser({user: "admin", pwd: "password", roles: ["__system"]});
    createUser(configAdminDB, noroleUser.user, noroleUser.roles);
    const results = runAuthorizationTestsOnAllInternalCommands(configPrimary, configAdminDB);
    shardingTest.stop();
    return results;
}

/**
 * singleCommandCheckResult gets the update from four setups(runStandaloneTest, sharded cluster,
 * sharded server, config server) and returns the combined result for the command.
 *
 *  * Parameters:
 *       commandName -- command name.
 *       resultmap -- an object that has setups as the keys and results for the setup as the values.
 *
 * Returns: a combined result for different test setups for a single command.
 */
function singleCommandCheckResult(commandName, resultmap) {
    let singleCommandCombinedResult = {pass: true, absent: true};
    for (const [setupName, resultsForSetup] of Object.entries(resultmap)) {
        if (!resultsForSetup.hasOwnProperty(commandName)) {
            continue;
        }
        if (singleCommandCombinedResult.pass) {
            singleCommandCombinedResult.pass = resultsForSetup[commandName].pass;
        }
        singleCommandCombinedResult.absent = false;
        if (!resultsForSetup[commandName].pass) {
            jsTestLog("Test Failure at setup:" + setupName + " command:" + commandName +
                      tojson(resultsForSetup[commandName]));
        }
    }
    return singleCommandCombinedResult;
}
/**
 * checkResults summarize the results from all the results from all the setups.
 *  * Parameters:
 *       resultmap -- an object that has setups as the keys and results for the setup as the values.
 *
 * Returns: None.
 *
 * The results for all the test objects are accumulated and a combined result for the all the tests
 * is announced.
 */
function checkResults(resultmap) {
    let summaryResultsCount = {passCount: 0, failCount: 0, absentCount: 0};
    for (const [commandName, testObject] of Object.entries(internalCommandsMap)) {
        if (skipCommand(testObject, commandName)) {
            continue;
        }
        const currentResult = singleCommandCheckResult(commandName, resultmap);
        if (currentResult.absent) {
            summaryResultsCount.absentCount++;
            jsTestLog(
                "Command ${commandName} did not get tested. This may be because a feature flag is not enabled, or this command does not exist in mongod and mongos.");
        } else if (currentResult.pass === true) {
            summaryResultsCount.passCount++;
        } else {
            jsTestLog("Test Result Failed command:" + tojson(currentResult));
            summaryResultsCount.failCount++;
        }
    }
    jsTest.log("Result: tests passed " + summaryResultsCount.passCount +
               ". Failed Commands Count: " + summaryResultsCount.failCount +
               ". Absent Commands Count: " + summaryResultsCount.absentCount + ".");
    assert.eq(0, summaryResultsCount.failCount);
}

let resultmap = {};
resultmap.Standalone = runStandaloneTest();
const opts = setupShardedClusterTest();
resultmap.ShardedClusterMongos = runMongosTest(opts);
resultmap.ShardedServer = runShardedServerTest(opts);
resultmap.ConfigServer = runConfigServer(opts);
checkResults(resultmap);
