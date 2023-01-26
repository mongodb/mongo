/**
 * This file defines tests for all existing commands and their expected behavior when run against a
 * node that is in the downgrading FCV state.
 *
 * Tagged as multiversion-incompatible as the list of commands will vary depending on version.
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

// This will verify the completeness of our map and run all tests.
load("jstests/libs/all_commands_test.js");
load("jstests/libs/fixture_helpers.js");  // For isSharded.
load("jstests/libs/feature_flag_util.js");

const name = jsTestName();
const dbName = "alltestsdb";
const collName = "alltestscoll";
const fullNs = dbName + "." + collName;

// Pre-written reasons for skipping a test.
const isAnInternalCommand = "internal command";
const isDeprecated = "deprecated command";
// TODO SERVER-69753 some commands we didn't have time for. Other commands are new in recent
// releases and don't make sense to test here.
const isNotImplementedYet = "not implemented yet";

let _lsid = UUID();
function getNextLSID() {
    _lsid = UUID();
    return {id: _lsid};
}
function getLSID() {
    return {id: _lsid};
}

// TODO SERVER-69753: Finish implementing commands marked as isNotImplementedYet.
const allCommands = {
    _addShard: {
        skip: isNotImplementedYet,
    },
    // TODO SERVER-69753: Make sure these internal commands are tested through passthrough suites.
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
    _configsvrCommitMergeAllChunksOnShard: {skip: isAnInternalCommand},
    _configsvrCommitMovePrimary: {skip: isAnInternalCommand},
    _configsvrCommitReshardCollection: {skip: isAnInternalCommand},
    _configsvrConfigureCollectionBalancing: {skip: isAnInternalCommand},
    _configsvrCreateDatabase: {skip: isAnInternalCommand},
    _configsvrDropIndexCatalogEntry: {skip: isAnInternalCommand},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isAnInternalCommand},
    _configsvrMoveChunk: {skip: isAnInternalCommand},
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
    _shardsvrDropCollectionIfUUIDNotMatching: {skip: isAnInternalCommand},
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
    _shardsvrMoveRange: {skip: isAnInternalCommand},
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
    _transferMods: {skip: isAnInternalCommand},
    _vectorClockPersist: {skip: isAnInternalCommand},
    abortReshardCollection: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // command: {abortReshardCollection: "test.x"},
        // isShardedOnly: true,
        // isAdminCommand: true
    },
    abortTransaction: {
        // TODO SERVER-69753: Uncomment/unskip and fix the command. Currently abortTransaction
        // cannot find the transaction number.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     assert.commandWorked(
        //         conn.getDB(dbName).runCommand({create: collName, writeConcern: {w:
        //         'majority'}}));
        //     // Ensure that the dbVersion is known.
        //     assert.commandWorked(
        //         conn.getCollection(fullNs).insert({x: 1}, {writeConcern: {w: "majority"}}));
        //     assert.eq(
        //         1, conn.getCollection(fullNs).find({x:
        //         1}).readConcern("local").limit(1).next().x);

        //     // Start the transaction.
        //     assert.commandWorked(conn.getDB(dbName).runCommand({
        //         insert: collName,
        //         documents: [{_id: ObjectId()}],
        //         lsid: getNextLSID(),
        //         stmtIds: [NumberInt(0)],
        //         txnNumber: NumberLong(0),
        //         startTransaction: true,
        //         autocommit: false,
        //     }));
        // },
        // command:
        //     {abortTransaction: 1, txnNumber: NumberLong(0), autocommit: false, lsid: getLSID()},
        // isAdminCommand: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // }
    },
    aggregate: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        // TODO SERVER-69753: Add additional aggregation stages to the pipeline to increase coverage
        // of all the agg stages.
        command: {aggregate: collName, pipeline: [{$match: {}}], cursor: {}},
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
        expectedErrorCode:
            6660400,  // Analyze command requires common query framework feature flag to be enabled.
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    analyzeShardKey: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        // },
        // command: {analyzeShardKey: fullNs, key: {skey: 1}},
        // isShardedOnly: true,
        // isAdminCommand: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
    },
    appendOplogNote: {command: {appendOplogNote: 1, data: {a: 1}}, isAdminCommand: true},
    applyOps: {
        command: {applyOps: []},
        isAdminCommand: true,
    },
    authenticate: {skip: isNotImplementedYet},
    autoSplitVector: {skip: isAnInternalCommand},
    buildInfo: {
        command: {buildInfo: 1},
        isAdminCommand: true,
    },
    bulkWrite: {skip: isNotImplementedYet},
    captrunc: {
        // TODO SERVER-69753: Uncomment/unskip and fix the command. Currently it is failing with the
        // same seg fault error as BF-26123.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     const db = conn.getDB(dbName);
        //     const coll = conn.getCollection(dbName + ".capped_truncate");
        //     assert.commandWorked(
        //         db.runCommand({create: "capped_truncate", capped: true, size: 1024}));
        //     for (let j = 1; j <= 10; j++) {
        //         assert.commandWorked(coll.insert({x: j}));
        //     }
        // },
        // command: {captrunc: "capped_truncate", n: 5, inc: false},
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: "capped_truncate"}));
        // }
    },
    checkMetadataConsistency: {skip: isNotImplementedYet},
    checkShardingIndex: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     const f = conn.getCollection(dbName + ".jstests_sharding_index");
        //     f.drop();
        //     f.createIndex({x: 1, y: 1});
        // },
        // command: {checkShardingIndex: dbName + ".jstests_sharding_index", keyPattern: {x: 1, y:
        // 1}},
        // isShardedOnly: true,
        // isShardSvrOnly: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop:
        //     "jstests_sharding_index"}));
        // }
    },
    cleanupOrphaned: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        //     for (let i = 0; i < 10; i++) {
        //         assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
        //     }
        // },
        // command: {cleanupOrphaned: fullNs},
        // isShardedOnly: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
    },
    cleanupReshardCollection: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // command: {cleanupReshardCollection: 1},
        // isShardedOnly: true,
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
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName + "2"}));
        },
    },
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterCount: {skip: isNotImplementedYet},
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
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // command: {commitReshardCollection: "test.x"},
        // isShardedOnly: true,
    },
    commitTransaction: {
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     assert.commandWorked(
        //         conn.getDB(dbName).runCommand({create: collName, writeConcern: {w:
        //         'majority'}}));
        //     // Ensure that the dbVersion is known.
        //     assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}, {writeConcern: {w:
        //     1}})); assert.eq(
        //         1, conn.getCollection(fullNs).find({x:
        //         1}).readConcern("local").limit(1).next().x);
        //     const lsid = assert.commandWorked(conn.getDB(dbName).runCommand({startSession:
        //     1})).id;
        //     // Start the transaction.
        //     assert.commandWorked(conn.getDB(dbName).runCommand({
        //         insert: collName,
        //         documents: [{_id: ObjectId()}],
        //         lsid: getNextLSID(),
        //         // stmtIds: [NumberInt(0)],
        //         txnNumber: NumberLong(0),
        //         startTransaction: true,
        //         autocommit: false
        //     }));
        // },
        // command:
        //     {commitTransaction: 1, txnNumber: NumberLong(0), autocommit: false, lsid: getLSID()},
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
        // isAdminCommand: true,
    },
    compact: {
        // TODO SERVER-69753: Uncomment the command and figure out way to skip this command when
        // running on an evergreen variant with the inMemory record store.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        // },
        // command: {compact: collName, force: true},
        // isReplSetOnly: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
    },
    compactStructuredEncryptionData: {skip: isNotImplementedYet},
    configureFailPoint: {skip: isAnInternalCommand},
    configureCollectionBalancing: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        // TODO SERVER-69753: Shard this collection in setUp
        //     assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        // },
        // command: {configureCollectionBalancing: collName},
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
        // isShardedOnly: true,
    },
    configureQueryAnalyzer: {
        isAdminCommand: true,
        command: {configureQueryAnalyzer: fullNs, mode: "full", sampleRate: 0.1},
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
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
            assert.commandWorked(conn.getCollection(fullNs).insert({x: 1}, {writeConcern: {w: 1}}));
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
    dbCheck: {command: {dbCheck: 1}},
    dbHash: {
        command: {dbHash: 1},
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
    donorWaitForMigrationToCommit: {skip: isAnInternalCommand},
    abortShardSplit: {skip: isAnInternalCommand},
    commitShardSplit: {skip: isAnInternalCommand},
    forgetShardSplit: {skip: isAnInternalCommand},
    driverOIDTest: {skip: isAnInternalCommand},
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
    dropConnections: {skip: isNotImplementedYet},
    dropDatabase: {skip: isNotImplementedYet},
    dropIndexes: {skip: isNotImplementedYet},
    dropRole: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createRole: "foo", privileges: [], roles: []}));
        },
        command: {dropRole: "foo"},
    },
    dropUser: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(dbName).runCommand({createUser: "foo", pwd: "bar", roles: []}));
        },
        command: {dropUser: "foo"},
    },
    echo: {skip: isNotImplementedYet},
    emptycapped: {
        command: {emptycapped: collName},
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
    },
    endSessions: {skip: isNotImplementedYet},
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
    flushRouterConfig: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // isShardedOnly: true,
        // isAdminCommand: true,
        // command: {flushRouterConfig: 1}
    },
    fsync: {
        skip: isNotImplementedYet,
    },
    fsyncUnlock: {
        skip: isNotImplementedYet,
    },
    getAuditConfig: {
        isAdminCommand: true,
        command: {getAuditConfig: 1},
    },
    getChangeStreamState: {
        isAdminCommand: true,
        command: {getChangeStreamState: 1},
        expectFailure: true,
        expectedErrorCode: ErrorCodes.CommandNotSupported  // only supported on serverless.
    },
    getClusterParameter: {
        isAdminCommand: true,
        command: {getClusterParameter: "changeStreamOptions"},
    },
    getCmdLineOpts: {
        isAdminCommand: true,
        command: {getCmdLineOpts: 1},
    },
    getDatabaseVersion: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // isAdminCommand: true,
        // command: {getDatabaseVersion: dbName},
        // isShardedOnly: true,
        // isShardSvrOnly: true,
    },
    getDefaultRWConcern: {
        isAdminCommand: true,
        command: {getDefaultRWConcern: 1},
    },
    getDiagnosticData: {
        isAdminCommand: true,
        command: {getDiagnosticData: 1},
    },
    getFreeMonitoringStatus: {
        isAdminCommand: true,
        command: {getFreeMonitoringStatus: 1},
    },
    getLog: {
        isAdminCommand: true,
        command: {getLog: "global"},
    },
    getMore: {
        // TODO SERVER-69753: Uncomment/unskip the command and fix it so that it passes. Currently
        // we need to be able to kill cursor ID in teardown in order for it to pass.
        skip: isNotImplementedYet,
        // setUp: function(conn) {
        //     const db = conn.getDB(dbName);
        //     for (let i = 0; i < 10; i++) {
        //         assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
        //     }

        //     const res = db.runCommand({find: collName, batchSize: 1});
        //     const cmdObj = {getMore: NumberLong(res.cursor.id), collection: collName};
        //     return cmdObj;
        // },
        // command: {},
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
    },
    getParameter: {isAdminCommand: true, command: {getParameter: 1, logLevel: 1}},
    getShardMap: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // isAdminCommand: true,
        // command: {getShardMap: 1},
        // isShardedOnly: true,
    },
    getShardVersion: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        //     setUp: function(conn) {
        //         assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        //     },
        //     command: {getShardVersion: collName},
        //     teardown: function(conn) {
        //         assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        //     },
        //     isShardedOnly: true,
    },
    godinsert: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        },
        teardown: function(conn) {
            assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        },
        command: {godinsert: collName, obj: {_id: 0, a: 0}},
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
    httpClientRequest: {skip: isNotImplementedYet},
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
    killOp: {skip: isNotImplementedYet},
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
    lockInfo: {skip: isNotImplementedYet, isAdminCommand: 1, command: {lockInfo: 1}},
    logApplicationMessage: {
        skip: isNotImplementedYet,
    },
    logMessage: {
        skip: isAnInternalCommand,
    },
    logRotate: {
        skip: isNotImplementedYet,
    },
    logout: {
        // TODO SERVER-69753: Implement this command so that it passes. It is currently failing with
        // "Each client connection may only be authenticated once.
        // Previously authenticated as: __system@local" error.
        skip: isNotImplementedYet,
    },
    makeSnapshot: {
        isAdminCommand: true,
        command: {makeSnapshot: 1},
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
        skip: isNotImplementedYet,
        isShardedOnly: true,
    },
    mergeChunks: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // isShardedOnly: true,
        // isAdminCommand: true,
        // setUp: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({create: collName}));
        //     assert.commandWorked(
        //         conn.getDB(dbName).adminCommand({shardCollection: fullNs, key: {_id: 1}}));
        //     for (let i = 0; i < 10; i++) {
        //         assert.commandWorked(conn.getCollection(fullNs).insert({a: i}));
        //     }
        // },
        // teardown: function(conn) {
        //     assert.commandWorked(conn.getDB(dbName).runCommand({drop: collName}));
        // },
        // command: {mergeChunks: fullNs, bounds: [{_id: MinKey}, {_id: MaxKey}]}
    },
    moveChunk: {
        skip: isNotImplementedYet,
        isShardedOnly: true,
    },
    moveRange: {
        skip: isNotImplementedYet,
        isShardedOnly: true,
    },
    oidcListKeys: {
        skip: isNotImplementedYet,
    },
    oidcRefreshKeys: {
        skip: isNotImplementedYet,
    },
    pinHistoryReplicated: {
        skip: isNotImplementedYet,
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
    prepareTransaction: {skip: isNotImplementedYet},
    profile: {
        skip: isNotImplementedYet,
    },
    reapLogicalSessionCacheNow: {
        isAdminCommand: true,
        command: {reapLogicalSessionCacheNow: 1},
    },
    recipientForgetMigration: {skip: isAnInternalCommand},
    recipientSyncData: {skip: isAnInternalCommand},
    recipientVoteImportedFiles: {skip: isAnInternalCommand},
    refreshLogicalSessionCacheNow: {skip: isNotImplementedYet},
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
    repairDatabase: {skip: isDeprecated},
    repairShardedCollectionChunksHistory: {skip: isAnInternalCommand},
    replSetAbortPrimaryCatchUp: {
        // TODO SERVER-69753: Uncomment this command and implement a way to use the ReplSetTest
        // fixture functions in the AllCommandsTest framework.
        skip: isNotImplementedYet,
        // setUp: function(conn, fixture) {
        //     var conf = fixture.getReplSetConfig();
        //     reconfigElectionAndCatchUpTimeout(fixture, 10000, -1);
        //     assert.commandWorked(conn.adminCommand(
        //         {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w:
        //         "majority"}}));
        //     let stepUpResults =
        //         stopReplicationAndEnforceNewPrimaryToCatchUp(fixture,
        //         fixture.getSecondaries()[0]);
        //     jsTestLog("Restarting server replication");
        //     restartServerReplication(stepUpResults.oldSecondaries);
        //     assert.commandWorked(conn.getDB("admin").runCommand({replSetStepDown: 60, force:
        //     true}));
        // },
        // isReplSetOnly: true,
        // command: {replSetAbortPrimaryCatchUp: 1},
        // isAdminCommand: true,
        // teardown: function(conn) {
        //     assert.commandWorked(conn.adminCommand(
        //         {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority"}, writeConcern: {w:
        //         "majority"}}));
        // }
    },
    replSetFreeze: {
        skip: isNotImplementedYet,  // can only run on secondary
        isReplSetOnly: true,
        isAdminCommand: true,
    },
    replSetGetConfig: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetConfig: 1}},
    replSetGetRBID: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetRBID: 1}},
    replSetGetStatus: {isReplSetOnly: true, isAdminCommand: true, command: {replSetGetStatus: 1}},
    replSetHeartbeat: {skip: isAnInternalCommand},
    replSetInitiate: {skip: isNotImplementedYet},
    replSetMaintenance: {skip: isNotImplementedYet},
    replSetReconfig: {skip: isNotImplementedYet},
    replSetRequestVotes: {skip: isNotImplementedYet},
    replSetStepDown: {
        skip: isNotImplementedYet,
    },
    replSetStepUp: {skip: isNotImplementedYet},
    replSetSyncFrom: {skip: isNotImplementedYet},
    replSetTest: {skip: isNotImplementedYet},
    replSetTestEgress: {skip: isNotImplementedYet},
    replSetUpdatePosition: {skip: isNotImplementedYet},
    replSetResizeOplog: {skip: isNotImplementedYet},
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
    rotateCertificates: {skip: isNotImplementedYet},
    saslContinue: {skip: isNotImplementedYet},
    saslStart: {skip: isNotImplementedYet},
    serverStatus: {
        isAdminCommand: true,
        command: {serverStatus: 1},
    },
    setAuditConfig: {skip: isNotImplementedYet},
    setCommittedSnapshot: {skip: isNotImplementedYet},
    setDefaultRWConcern: {skip: isNotImplementedYet},
    setIndexCommitQuorum: {skip: isNotImplementedYet},
    setFeatureCompatibilityVersion: {skip: isNotImplementedYet},
    setFreeMonitoring: {skip: isNotImplementedYet},
    setProfilingFilterGlobally: {skip: isNotImplementedYet},
    setParameter: {skip: isNotImplementedYet},
    setShardVersion: {skip: isNotImplementedYet},
    setChangeStreamState: {skip: isNotImplementedYet},
    setClusterParameter: {skip: isNotImplementedYet},
    setUserWriteBlockMode: {skip: isNotImplementedYet},
    shardingState: {skip: isNotImplementedYet},
    shutdown: {skip: isNotImplementedYet},
    sleep: {skip: isNotImplementedYet},
    splitChunk: {skip: isNotImplementedYet},
    splitVector: {skip: isNotImplementedYet},
    stageDebug: {skip: isNotImplementedYet},
    startRecordingTraffic: {skip: isNotImplementedYet},
    startSession: {skip: isNotImplementedYet},
    stopRecordingTraffic: {skip: isNotImplementedYet},
    testDeprecation: {skip: isNotImplementedYet},
    testDeprecationInVersion2: {skip: isNotImplementedYet},
    testInternalTransactions: {skip: isAnInternalCommand},
    testRemoval: {skip: isNotImplementedYet},
    testReshardCloneCollection: {skip: isNotImplementedYet},
    testVersions1And2: {skip: isNotImplementedYet},
    testVersion2: {skip: isNotImplementedYet},
    top: {
        command: {top: 1},
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
    voteCommitImportCollection: {skip: isAnInternalCommand},
    voteCommitIndexBuild: {skip: isAnInternalCommand},
    waitForFailPoint: {
        skip: isAnInternalCommand,
    },
    waitForOngoingChunkSplits: {
        // TODO SERVER-69753: Unskip this command when we can test with sharded clusters.
        skip: isNotImplementedYet,
        // command: {waitForOngoingChunkSplits: 1},
        // isShardedOnly: true
    },
    whatsmysni: {
        command: {whatsmysni: 1},
        isAdminCommand: true,
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

let runAllCommandsTest = function(test, conn) {
    let cmdDb = conn.getDB(dbName);

    if (test.isShardedOnly && !isMongos(cmdDb)) {
        jsTestLog("Skipping " + tojson(test.command) + " because it is for sharded clusters only");
        return;
    }
    if (test.isReplSetOnly && isMongos(cmdDb)) {
        jsTestLog("Skipping " + tojson(test.command) + " because it is for replica sets only");
        return;
    }

    let cmdObj = test.command;
    if (typeof (test.setUp) === "function") {
        let setUpRes = test.setUp(conn);

        // For some commands (such as killSessions) the command requires information that is
        // created during the setUp portion (such as a session ID), so we need to create the
        // command in setUp. We set the command to an empty object in order to indicate that
        // the command created in setUp should be used instead.
        if (Object.keys(test.command).length === 0) {
            cmdObj = setUpRes;
        }
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

    if (typeof (test.teardown) === "function") {
        test.teardown(conn);
    }
};

let runTest = function(conn, adminDB) {
    let runDowngradingToUpgrading = false;
    if (FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        runDowngradingToUpgrading = true;
    }
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));

    assert.commandFailed(conn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    jsTestLog("Running all commands in the downgradingToLastLTS FCV");
    AllCommandsTest.testAllCommands(conn, allCommands, runAllCommandsTest);

    if (runDowngradingToUpgrading) {
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

        jsTestLog("Running all commands after upgrading back to the latest FCV");
        AllCommandsTest.testAllCommands(conn, allCommands, runAllCommandsTest);
    }
};

const rst = new ReplSetTest({name: name, nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryAdminDB = primary.getDB("admin");

runTest(primary, primaryAdminDB);

rst.stopSet();

// TODO SERVER-69753: Run runTest with a sharded cluster as well
})();
