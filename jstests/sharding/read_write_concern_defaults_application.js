/**
 * Tests that commands correctly apply read/write concern defaults.
 *
 * The following fields are required for each command that is not skipped:
 *
 * - setUp: [OPTIONAL] A function that does any set up (inserts, etc.) needed to check the command's
 *   results. These operations will run with the default RWC set, so ensure they use appropriate
 *   explicit RWC if necessary.
 * - command: The command to run, with all required options. If a function, is called with the
 *   connection as the argument, and the returned object is used. The readConcern/writeConcern
 *   fields may be appended to this object. The command object cannot include its own explicit
 *   readConcern/writeConcern fields.
 * - checkReadConcern: Boolean that controls whether to check the application of readConcern.
 * - checkWriteConcern: Boolean that controls whether to check the application of writeConcern.
 * - db: [OPTIONAL] The database to run the command against.
 * - target: [OPTIONAL] If set to "sharded", command is only valid on sharded clusters. If set to
 *   "replset", command is only valid on non-sharded clusters. If omitted, command is valid on
 *   sharded and non-sharded clusters.
 * - shardedTargetsConfigServer: [OPTIONAL] When sharded, the operation is sent to the config
 *   server, rather than to the shard.
 * - useLogs: Normally, profiling is used to check behavior.  Set this to true to use slow op log
 *   lines instead.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_majority_read_concern,
 *   requires_profiling,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/profiler.js');
load("jstests/libs/logv2_helpers.js");
load('jstests/sharding/libs/last_lts_mongod_commands.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

let db = "test";
let coll = "foo";
let nss = db + "." + coll;

let _lsid = UUID();
function getNextLSID() {
    _lsid = UUID();
    return {id: _lsid};
}
function getLSID() {
    return {id: _lsid};
}

// Check that a test case is well-formed.
let validateTestCase = function(test) {
    if ("setUp" in test) {
        assert(typeof (test.setUp) === "function");
    }
    assert("command" in test &&
           (typeof (test.command) === "object" || typeof (test.command) === "function"));
    assert("checkReadConcern" in test && typeof (test.checkReadConcern) === "boolean");
    assert("checkWriteConcern" in test && typeof (test.checkWriteConcern) === "boolean");
    if ("db" in test) {
        assert(typeof (test.db) === "string");
    }
    if ("target" in test) {
        assert(test.target === "replset" || test.target === "sharded");
    }
    if ("shardedTargetsConfigServer" in test) {
        assert(typeof (test.shardedTargetsConfigServer) === "boolean");
    }
    if ("useLogs" in test) {
        assert(typeof (test.useLogs) === "boolean");
    }
};

let testCases = {
    _addShard: {skip: "internal command"},
    _cloneCollectionOptionsFromPrimaryShard: {skip: "internal command"},
    _configsvrAbortReshardCollection: {skip: "internal command"},
    _configsvrAddShard: {skip: "internal command"},
    _configsvrAddShardToZone: {skip: "internal command"},
    _configsvrBalancerCollectionStatus: {skip: "internal command"},
    _configsvrBalancerStart: {skip: "internal command"},
    _configsvrBalancerStatus: {skip: "internal command"},
    _configsvrBalancerStop: {skip: "internal command"},
    _configsvrCleanupReshardCollection: {skip: "internal command"},
    _configsvrCollMod: {skip: "internal command"},
    _configsvrClearJumboFlag: {skip: "internal command"},
    _configsvrCommitChunksMerge: {skip: "internal command"},
    _configsvrCommitChunkMigration: {skip: "internal command"},
    _configsvrCommitChunkSplit: {skip: "internal command"},
    _configsvrCommitIndex: {skip: "internal command"},
    _configsvrCommitMovePrimary: {skip: "internal command"},  // Can be removed once 6.0 is last LTS
    _configsvrCommitReshardCollection: {skip: "internal command"},
    _configsvrConfigureAutoSplit: {
        skip: "internal command"
    },  // TODO SERVER-62374: remove this once 5.3 becomes last continuos release
    _configsvrConfigureCollectionBalancing: {skip: "internal command"},
    _configsvrCreateDatabase: {skip: "internal command"},
    _configsvrDropCollection:
        {skip: "internal command"},  // TODO SERVER-58843: Remove once 6.0 becomes last LTS
    _configsvrDropDatabase:
        {skip: "internal command"},  // TODO SERVER-58843: Remove once 6.0 becomes last LTS
    _configsvrDropIndexCatalogEntry: {skip: "internal command"},
    _configsvrEnableSharding:
        {skip: "internal command"},  // TODO SERVER-58843: Remove once 6.0 becomes last LTS
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: "internal command"},
    _configsvrMoveChunk: {skip: "internal command"},
    _configsvrMovePrimary: {skip: "internal command"},  // Can be removed once 6.0 is last LTS
    _configsvrMoveRange: {skip: "internal command"},
    _configsvrRefineCollectionShardKey: {skip: "internal command"},
    _configsvrRemoveChunks: {skip: "internal command"},
    _configsvrRemoveShard: {skip: "internal command"},
    _configsvrRemoveShardFromZone: {skip: "internal command"},
    _configsvrRemoveTags: {skip: "internal command"},
    _configsvrRenameCollection: {skip: "internal command"},
    _configsvrRenameCollectionMetadata: {skip: "internal command"},
    _configsvrRepairShardedCollectionChunksHistory: {skip: "internal command"},
    _configsvrReshardCollection: {skip: "internal command"},
    _configsvrRunRestore: {skip: "internal command"},
    _configsvrSetAllowMigrations: {skip: "internal command"},
    _configsvrSetClusterParameter: {skip: "internal command"},
    _configsvrSetUserWriteBlockMode: {skip: "internal command"},
    _configsvrShardCollection:
        {skip: "internal command"},  // TODO SERVER-58843: Remove once 6.0 becomes last LTS
    _configsvrUpdateZoneKeyRange: {skip: "internal command"},
    _flushDatabaseCacheUpdates: {skip: "internal command"},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _flushReshardingStateChange: {skip: "internal command"},
    _flushRoutingTableCacheUpdates: {skip: "internal command"},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: "internal command"},
    _getAuditConfigGeneration: {skip: "does not accept read or write concern"},
    _getNextSessionMods: {skip: "internal command"},
    _getUserCacheGeneration: {skip: "internal command"},
    _hashBSONElement: {skip: "internal command"},
    _isSelf: {skip: "internal command"},
    _killOperations: {skip: "internal command"},
    _mergeAuthzCollections: {skip: "internal command"},
    _migrateClone: {skip: "internal command"},
    _recvChunkAbort: {skip: "internal command"},
    _recvChunkCommit: {skip: "internal command"},
    _recvChunkReleaseCritSec: {skip: "internal command"},
    _recvChunkStart: {skip: "internal command"},
    _recvChunkStatus: {skip: "internal command"},
    _shardsvrAbortReshardCollection: {skip: "internal command"},
    _shardsvrCleanupReshardCollection: {skip: "internal command"},
    _shardsvrCloneCatalogData: {skip: "internal command"},
    _shardsvrRegisterIndex: {skip: "internal command"},
    _shardsvrCommitIndexParticipant: {skip: "internal command"},
    _shardsvrCommitReshardCollection: {skip: "internal command"},
    _shardsvrCompactStructuredEncryptionData: {skip: "internal command"},
    _shardsvrCreateCollection: {skip: "internal command"},
    _shardsvrCreateCollectionParticipant: {skip: "internal command"},
    _shardsvrDropCollection: {skip: "internal command"},
    _shardsvrDropCollectionIfUUIDNotMatching: {skip: "internal command"},
    _shardsvrDropCollectionParticipant: {skip: "internal command"},
    _shardsvrUnregisterIndex: {skip: "internal command"},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: "internal command"},
    _shardsvrDropIndexes: {skip: "internal command"},
    _shardsvrDropDatabase: {skip: "internal command"},
    _shardsvrDropDatabaseParticipant: {skip: "internal command"},
    _shardsvrGetStatsForBalancing: {skip: "internal command"},
    _shardsvrJoinMigrations: {skip: "internal command"},
    _shardsvrMovePrimary: {skip: "internal command"},
    _shardsvrMoveRange: {
        skip:
            "does not accept read or write concern (accepts writeConcern, but only explicitly and when _secondaryThrottle is true)"
    },
    _shardsvrRefineCollectionShardKey: {skip: "internal command"},
    _shardsvrRenameCollection: {skip: "internal command"},
    _shardsvrRenameCollectionParticipant: {skip: "internal command"},
    _shardsvrRenameCollectionParticipantUnblock: {skip: "internal command"},
    _shardsvrReshardCollection: {skip: "internal command"},
    _shardsvrReshardingOperationTime: {skip: "internal command"},
    _shardsvrSetAllowMigrations: {skip: "internal command"},
    _shardsvrSetClusterParameter: {skip: "internal command"},
    _shardsvrSetUserWriteBlockMode: {skip: "internal command"},
    _shardsvrCollMod: {skip: "internal command"},
    _shardsvrCollModParticipant: {skip: "internal command"},
    _shardsvrParticipantBlock: {skip: "internal command"},
    _shardsvrShardCollection:
        {skip: "internal command"},  // TODO SERVER-58843: Remove once 6.0 becomes last LTS
    _transferMods: {skip: "internal command"},
    _vectorClockPersist: {skip: "internal command"},
    abortReshardCollection: {skip: "does not accept read or write concern"},
    abortTransaction: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(db).runCommand({create: coll, writeConcern: {w: 'majority'}}));
            // Ensure that the dbVersion is known.
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
            assert.eq(1,
                      conn.getCollection(nss).find({x: 1}).readConcern("local").limit(1).next().x);
            // Start the transaction.
            assert.commandWorked(conn.getDB(db).runCommand({
                insert: coll,
                documents: [{_id: ObjectId()}],
                lsid: getNextLSID(),
                stmtIds: [NumberInt(0)],
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false
            }));
        },
        command: () =>
            ({abortTransaction: 1, txnNumber: NumberLong(0), autocommit: false, lsid: getLSID()}),
        db: "admin",
        checkReadConcern: false,
        checkWriteConcern: true,
        useLogs: true,
    },
    addShard: {skip: "does not accept read or write concern"},
    addShardToZone: {skip: "does not accept read or write concern"},
    aggregate: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {aggregate: coll, pipeline: [{$match: {x: 1}}, {$out: "out"}], cursor: {}},
        checkReadConcern: true,
        checkWriteConcern: true,
    },
    analyze: {skip: "TODO SERVER-67772"},
    analyzeShardKey: {skip: "does not accept read or write concern"},
    appendOplogNote: {
        command: {appendOplogNote: 1, data: {foo: 1}},
        checkReadConcern: false,
        checkWriteConcern: true,
        target: "replset",
        db: "admin",
        useLogs: true,
    },
    applyOps: {skip: "internal command"},
    authenticate: {skip: "does not accept read or write concern"},
    autoSplitVector: {skip: "internal command"},
    balancerCollectionStatus: {skip: "does not accept read or write concern"},
    balancerStart: {skip: "does not accept read or write concern"},
    balancerStatus: {skip: "does not accept read or write concern"},
    balancerStop: {skip: "does not accept read or write concern"},
    buildInfo: {skip: "does not accept read or write concern"},
    captrunc: {skip: "test command"},
    checkShardingIndex: {skip: "does not accept read or write concern"},
    cleanupOrphaned: {skip: "only on shard server"},
    cleanupReshardCollection: {skip: "does not accept read or write concern"},
    clearJumboFlag: {skip: "does not accept read or write concern"},
    clearLog: {skip: "does not accept read or write concern"},
    clone: {skip: "deprecated"},
    cloneCollectionAsCapped: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {cloneCollectionAsCapped: coll, toCollection: coll + "2", size: 10 * 1024 * 1024},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    clusterAbortTransaction: {skip: "already tested by 'abortTransaction' tests on mongos"},
    clusterAggregate: {skip: "already tested by 'aggregate' tests on mongos"},
    clusterCommitTransaction: {skip: "already tested by 'commitTransaction' tests on mongos"},
    clusterDelete: {skip: "already tested by 'delete' tests on mongos"},
    clusterFind: {skip: "already tested by 'find' tests on mongos"},
    clusterGetMore: {skip: "already tested by 'getMore' tests on mongos"},
    clusterInsert: {skip: "already tested by 'insert' tests on mongos"},
    clusterUpdate: {skip: "already tested by 'update' tests on mongos"},
    collMod: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {collMod: coll, validator: {}},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    collStats: {skip: "does not accept read or write concern"},
    commitReshardCollection: {skip: "does not accept read or write concern"},
    commitTransaction: {
        setUp: function(conn) {
            assert.commandWorked(
                conn.getDB(db).runCommand({create: coll, writeConcern: {w: 'majority'}}));
            // Ensure that the dbVersion is known.
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
            assert.eq(1,
                      conn.getCollection(nss).find({x: 1}).readConcern("local").limit(1).next().x);
            // Start the transaction.
            assert.commandWorked(conn.getDB(db).runCommand({
                insert: coll,
                documents: [{_id: ObjectId()}],
                lsid: getNextLSID(),
                stmtIds: [NumberInt(0)],
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false
            }));
        },
        command: () =>
            ({commitTransaction: 1, txnNumber: NumberLong(0), autocommit: false, lsid: getLSID()}),
        db: "admin",
        checkReadConcern: false,
        checkWriteConcern: true,
        useLogs: true,
    },
    compact: {skip: "does not accept read or write concern"},
    compactStructuredEncryptionData: {skip: "does not accept read or write concern"},
    configureCollectionAutoSplitter: {
        skip: "does not accept read or write concern"
    },  // TODO SERVER-62374: remove this once 5.3 becomes last continuos release
    configureCollectionBalancing: {skip: "does not accept read or write concern"},
    configureFailPoint: {skip: "does not accept read or write concern"},
    configureQueryAnalyzer: {skip: "does not accept read or write concern"},
    connPoolStats: {skip: "does not accept read or write concern"},
    connPoolSync: {skip: "internal command"},
    connectionStatus: {skip: "does not accept read or write concern"},
    convertToCapped: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {convertToCapped: coll, size: 10 * 1024 * 1024},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    coordinateCommitTransaction: {skip: "internal command"},
    count: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {count: coll, query: {x: 1}},
        checkReadConcern: true,
        checkWriteConcern: false,
    },
    cpuload: {skip: "does not accept read or write concern"},
    create: {
        command: {create: coll},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    createIndexes: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {createIndexes: coll, indexes: [{key: {x: 1}, name: "foo"}]},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    createRole: {
        command: {createRole: "foo", privileges: [], roles: []},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    createUser: {
        command: {createUser: "foo", pwd: "bar", roles: []},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    currentOp: {skip: "does not accept read or write concern"},
    dataSize: {skip: "does not accept read or write concern"},
    dbCheck: {skip: "does not accept read or write concern"},
    dbHash: {skip: "does not accept read or write concern"},
    dbStats: {skip: "does not accept read or write concern"},
    delete: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {delete: coll, deletes: [{q: {x: 1}, limit: 1}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        // TODO SERVER-23266: If the overall batch command if profiled, then it would be better to
        // use profiling.  In the meantime, use logs.
        useLogs: true,
    },
    distinct: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {distinct: coll, key: "x"},
        checkReadConcern: true,
        checkWriteConcern: false,
    },
    donorAbortMigration: {skip: "does not accept read or write concern"},
    donorForgetMigration: {skip: "does not accept read or write concern"},
    donorStartMigration: {skip: "does not accept read or write concern"},
    donorWaitForMigrationToCommit: {skip: "does not accept read or write concern"},
    abortShardSplit: {skip: "internal command"},
    commitShardSplit: {skip: "internal command"},
    forgetShardSplit: {skip: "internal command"},
    driverOIDTest: {skip: "internal command"},
    drop: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {drop: coll},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    dropAllRolesFromDatabase: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
        },
        command: {dropAllRolesFromDatabase: 1},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    dropAllUsersFromDatabase: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}));
        },
        command: {dropAllUsersFromDatabase: 1},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    dropConnections: {skip: "does not accept read or write concern"},
    dropDatabase: {skip: "not profiled or logged"},
    dropIndexes: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
            assert.commandWorked(conn.getDB(db).runCommand({
                createIndexes: coll,
                indexes: [{key: {x: 1}, name: "foo"}],
                writeConcern: {w: 1}
            }));
        },
        command: {dropIndexes: coll, index: "foo"},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    dropRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
        },
        command: {dropRole: "foo"},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    dropUser: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}));
        },
        command: {dropUser: "foo"},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    echo: {skip: "does not accept read or write concern"},
    emptycapped: {skip: "test command"},
    enableSharding: {skip: "does not accept read or write concern"},
    endSessions: {skip: "does not accept read or write concern"},
    explain: {skip: "TODO SERVER-45478"},
    features: {skip: "internal command"},
    filemd5: {skip: "does not accept read or write concern"},
    find: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {find: coll, filter: {x: 1}},
        checkReadConcern: true,
        checkWriteConcern: false,
    },
    findAndModify: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {findAndModify: coll, query: {x: 1}, update: {$set: {x: 2}}},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    flushRouterConfig: {skip: "does not accept read or write concern"},
    forceerror: {skip: "test command"},
    fsync: {skip: "does not accept read or write concern"},
    fsyncUnlock: {skip: "does not accept read or write concern"},
    getAuditConfig: {skip: "does not accept read or write concern"},
    getChangeStreamState: {skip: "does not accept read or write concern"},
    getClusterParameter: {skip: "does not accept read or write concern"},
    getCmdLineOpts: {skip: "does not accept read or write concern"},
    getDatabaseVersion: {skip: "does not accept read or write concern"},
    getDefaultRWConcern: {skip: "does not accept read or write concern"},
    getDiagnosticData: {skip: "does not accept read or write concern"},
    getFreeMonitoringStatus: {skip: "does not accept read or write concern"},
    getLastError: {skip: "does not accept read or write concern"},
    getLog: {skip: "does not accept read or write concern"},
    getMore: {skip: "does not accept read or write concern"},
    getParameter: {skip: "does not accept read or write concern"},
    getShardMap: {skip: "internal command"},
    getShardVersion: {skip: "internal command"},
    getnonce: {skip: "does not accept read or write concern"},
    godinsert: {skip: "for testing only"},
    grantPrivilegesToRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
        },
        command: {
            grantPrivilegesToRole: "foo",
            privileges: [{resource: {db: db, collection: coll}, actions: ["find"]}]
        },
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    grantRolesToRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "bar", privileges: [], roles: [], writeConcern: {w: 1}}));
        },
        command: {grantRolesToRole: "foo", roles: [{role: "bar", db: db}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    grantRolesToUser: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
            assert.commandWorked(conn.getDB(db).runCommand(
                {createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}));
        },
        command: {grantRolesToUser: "foo", roles: [{role: "foo", db: db}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    handshake: {skip: "does not accept read or write concern"},
    hello: {skip: "does not accept read or write concern"},
    hostInfo: {skip: "does not accept read or write concern"},
    httpClientRequest: {skip: "does not accept read or write concern"},
    exportCollection: {skip: "internal command"},
    importCollection: {skip: "internal command"},
    insert: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {insert: coll, documents: [{_id: ObjectId()}]},
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: "internal command"},
    invalidateUserCache: {skip: "does not accept read or write concern"},
    isdbgrid: {skip: "does not accept read or write concern"},
    isMaster: {skip: "does not accept read or write concern"},
    killAllSessions: {skip: "does not accept read or write concern"},
    killAllSessionsByPattern: {skip: "does not accept read or write concern"},
    killCursors: {skip: "does not accept read or write concern"},
    killOp: {skip: "does not accept read or write concern"},
    killSessions: {skip: "does not accept read or write concern"},
    listCollections: {skip: "does not accept read or write concern"},
    listCommands: {skip: "does not accept read or write concern"},
    listDatabases: {skip: "does not accept read or write concern"},
    listDatabasesForAllTenants: {skip: "does not accept read or write concern"},
    listIndexes: {skip: "does not accept read or write concern"},
    listShards: {skip: "does not accept read or write concern"},
    lockInfo: {skip: "does not accept read or write concern"},
    logApplicationMessage: {skip: "does not accept read or write concern"},
    logMessage: {skip: "does not accept read or write concern"},
    logRotate: {skip: "does not accept read or write concern"},
    logout: {skip: "does not accept read or write concern"},
    makeSnapshot: {skip: "does not accept read or write concern"},
    mapReduce: {skip: "does not accept read or write concern"},
    mergeChunks: {skip: "does not accept read or write concern"},
    moveChunk: {
        skip:
            "does not accept read or write concern (accepts writeConcern, but only explicitly and when _secondaryThrottle is true)"
    },
    movePrimary: {skip: "does not accept read or write concern"},
    moveRange: {
        skip:
            "does not accept read or write concern (accepts writeConcern, but only explicitly and when _secondaryThrottle is true)"
    },
    multicast: {skip: "does not accept read or write concern"},
    netstat: {skip: "internal command"},
    pinHistoryReplicated: {skip: "internal command"},
    ping: {skip: "does not accept read or write concern"},
    planCacheClear: {skip: "does not accept read or write concern"},
    planCacheClearFilters: {skip: "does not accept read or write concern"},
    planCacheListFilters: {skip: "does not accept read or write concern"},
    planCacheSetFilter: {skip: "does not accept read or write concern"},
    prepareTransaction: {skip: "internal command"},
    profile: {skip: "does not accept read or write concern"},
    reIndex: {skip: "does not accept read or write concern"},
    reapLogicalSessionCacheNow: {skip: "does not accept read or write concern"},
    recipientForgetMigration: {skip: "does not accept read or write concern"},
    recipientSyncData: {skip: "does not accept read or write concern"},
    recipientVoteImportedFiles: {skip: "does not accept read or write concern"},
    refineCollectionShardKey: {skip: "does not accept read or write concern"},
    refreshLogicalSessionCacheNow: {skip: "does not accept read or write concern"},
    refreshSessions: {skip: "does not accept read or write concern"},
    refreshSessionsInternal: {skip: "internal command"},
    removeShard: {skip: "does not accept read or write concern"},
    removeShardFromZone: {skip: "does not accept read or write concern"},
    renameCollection: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({create: coll, writeConcern: {w: 1}}));
        },
        command: {renameCollection: nss, to: nss + "2"},
        db: "admin",
        target: "replset",
        checkReadConcern: false,
        checkWriteConcern: true,
    },
    repairDatabase: {skip: "does not accept read or write concern"},
    repairShardedCollectionChunksHistory: {skip: "does not accept read or write concern"},
    replSetAbortPrimaryCatchUp: {skip: "does not accept read or write concern"},
    replSetFreeze: {skip: "does not accept read or write concern"},
    replSetGetConfig: {skip: "does not accept read or write concern"},
    replSetGetRBID: {skip: "does not accept read or write concern"},
    replSetGetStatus: {skip: "does not accept read or write concern"},
    replSetHeartbeat: {skip: "does not accept read or write concern"},
    replSetInitiate: {skip: "does not accept read or write concern"},
    replSetMaintenance: {skip: "does not accept read or write concern"},
    replSetReconfig: {skip: "does not accept read or write concern"},
    replSetRequestVotes: {skip: "does not accept read or write concern"},
    replSetResizeOplog: {skip: "does not accept read or write concern"},
    replSetStepDown: {skip: "does not accept read or write concern"},
    replSetStepUp: {skip: "does not accept read or write concern"},
    replSetSyncFrom: {skip: "does not accept read or write concern"},
    replSetTest: {skip: "does not accept read or write concern"},
    replSetTestEgress: {skip: "does not accept read or write concern"},
    replSetUpdatePosition: {skip: "does not accept read or write concern"},
    reshardCollection: {skip: "does not accept read or write concern"},
    resync: {skip: "does not accept read or write concern"},
    revokePrivilegesFromRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand({
                createRole: "foo",
                privileges: [{resource: {db: db, collection: coll}, actions: ["find"]}],
                roles: [],
                writeConcern: {w: 1}
            }));
        },
        command: {
            revokePrivilegesFromRole: "foo",
            privileges: [{resource: {db: db, collection: coll}, actions: ["find"]}]
        },
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    revokeRolesFromRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "bar", privileges: [], roles: [], writeConcern: {w: 1}}));
            assert.commandWorked(conn.getDB(db).runCommand({
                createRole: "foo",
                privileges: [],
                roles: [{role: "bar", db: db}],
                writeConcern: {w: 1}
            }));
        },
        command: {revokeRolesFromRole: "foo", roles: [{role: "foo", db: db}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    revokeRolesFromUser: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
            assert.commandWorked(conn.getDB(db).runCommand({
                createUser: "foo",
                pwd: "bar",
                roles: [{role: "foo", db: db}],
                writeConcern: {w: 1}
            }));
        },
        command: {revokeRolesFromUser: "foo", roles: [{role: "foo", db: db}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    rolesInfo: {skip: "does not accept read or write concern"},
    rotateCertificates: {skip: "does not accept read or write concern"},
    saslContinue: {skip: "does not accept read or write concern"},
    saslStart: {skip: "does not accept read or write concern"},
    sbe: {skip: "internal command"},
    serverStatus: {skip: "does not accept read or write concern"},
    setAllowMigrations: {skip: "does not accept read or write concern"},
    setAuditConfig: {skip: "does not accept read or write concern"},
    setCommittedSnapshot: {skip: "internal command"},
    setDefaultRWConcern: {skip: "special case (must run after all other commands)"},
    setFeatureCompatibilityVersion: {skip: "does not accept read or write concern"},
    setFreeMonitoring: {skip: "does not accept read or write concern"},
    setIndexCommitQuorum: {skip: "does not accept read or write concern"},
    setParameter: {skip: "does not accept read or write concern"},
    setShardVersion: {skip: "internal command"},
    setChangeStreamState: {skip: "does not accept read or write concern"},
    setClusterParameter: {skip: "does not accept read or write concern"},
    setUserWriteBlockMode: {skip: "does not accept read or write concern"},
    shardCollection: {skip: "does not accept read or write concern"},
    shardingState: {skip: "does not accept read or write concern"},
    shutdown: {skip: "does not accept read or write concern"},
    sleep: {skip: "does not accept read or write concern"},
    split: {skip: "does not accept read or write concern"},
    splitChunk: {skip: "does not accept read or write concern"},
    splitVector: {skip: "internal command"},
    stageDebug: {skip: "does not accept read or write concern"},
    startRecordingTraffic: {skip: "does not accept read or write concern"},
    startSession: {skip: "does not accept read or write concern"},
    stopRecordingTraffic: {skip: "does not accept read or write concern"},
    testDeprecation: {skip: "does not accept read or write concern"},
    testDeprecationInVersion2: {skip: "does not accept read or write concern"},
    testInternalTransactions: {skip: "internal command"},
    testRemoval: {skip: "does not accept read or write concern"},
    testReshardCloneCollection: {skip: "internal command"},
    testVersions1And2: {skip: "does not accept read or write concern"},
    testVersion2: {skip: "does not accept read or write concern"},
    top: {skip: "does not accept read or write concern"},
    update: {
        setUp: function(conn) {
            assert.commandWorked(conn.getCollection(nss).insert({x: 1}, {writeConcern: {w: 1}}));
        },
        command: {update: coll, updates: [{q: {x: 1}, u: {x: 2}}]},
        checkReadConcern: false,
        checkWriteConcern: true,
        // TODO SERVER-23266: If the overall batch command if profiled, then it would be better to
        // use profiling.  In the meantime, use logs.
        useLogs: true,
    },
    updateRole: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createRole: "foo", privileges: [], roles: [], writeConcern: {w: 1}}));
        },
        command: {updateRole: "foo", privileges: []},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    updateUser: {
        setUp: function(conn) {
            assert.commandWorked(conn.getDB(db).runCommand(
                {createUser: "foo", pwd: "bar", roles: [], writeConcern: {w: 1}}));
        },
        command: {updateUser: "foo", pwd: "bar2"},
        checkReadConcern: false,
        checkWriteConcern: true,
        shardedTargetsConfigServer: true,
        useLogs: true,
    },
    updateZoneKeyRange: {skip: "does not accept read or write concern"},
    usersInfo: {skip: "does not accept read or write concern"},
    validate: {skip: "does not accept read or write concern"},
    validateDBMetadata: {skip: "does not accept read or write concern"},
    voteCommitImportCollection: {skip: "internal command"},
    voteCommitIndexBuild: {skip: "internal command"},
    waitForFailPoint: {skip: "does not accept read or write concern"},
    waitForOngoingChunkSplits: {skip: "does not accept read or write concern"},
    whatsmysni: {skip: "does not accept read or write concern"},
    whatsmyuri: {skip: "internal command"},
};

commandsRemovedFromMongodSinceLastLTS.forEach(function(cmd) {
    testCases[cmd] = {skip: "must define test coverage for backwards compatibility"};
});

// Running setDefaultRWConcern in the middle of a scenario would define defaults when there
// shouldn't be for subsequently-tested commands. Thus it is special-cased to be run at the end of
// the scenario.
let setDefaultRWConcernActualTestCase = {
    command: function(conn) {
        let currentDefaults = assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1}));
        let res = {setDefaultRWConcern: 1};
        if ("defaultReadConcern" in currentDefaults) {
            res = Object.extend(res, {defaultReadConcern: currentDefaults.defaultReadConcern});
        }
        if ("defaultWriteConcern" in currentDefaults) {
            res = Object.extend(res, {defaultWriteConcern: currentDefaults.defaultWriteConcern});
        }
        if (!("defaultReadConcern" in currentDefaults) &&
            !("defaultWriteConcern" in currentDefaults)) {
            res = Object.extend(res, {defaultWriteConcern: {w: 1}});
        }
        return res;
    },
    db: "admin",
    checkReadConcern: false,
    checkWriteConcern: true,
    shardedTargetsConfigServer: true,
    useLogs: true,
};

// Example log line (broken over several lines), indicating the sections matched by the regex
// generated by this function:
// 2020-01-28T16:46:57.373+1100 I  COMMAND  [conn18] command test.foo appName: "MongoDB Shell"
//      command: aggregate { aggregate: "foo", ..., comment: "5e2fcad149034875e22c9fbc", ... }
//     ^^^^^^^^^^^^^^^^^^^                         ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//      planSummary: COLLSCAN keysExamined:0 docsExamined:1 ... readConcern:{ level: "majority" }
//                                                             ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//      writeConcern:{ w: "majority", wtimeout: 1234567 } storage:{} protocol:op_msg 275ms
//     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
function createLogLineRegularExpressionForTestCase(test, cmdName, targetId, explicitRWC) {
    let expectedProvenance = explicitRWC ? "clientSupplied" : "customDefault";
    if (isJsonLogNoConn()) {
        let pattern = `"command":{"${cmdName}"`;
        pattern += `.*"comment":"${targetId}"`;
        if (test.checkReadConcern) {
            pattern += `.*"readConcern":{"level":"majority","provenance":"${expectedProvenance}"}`;
        }
        if (test.checkWriteConcern) {
            pattern += `.*"writeConcern":{"w":"majority","wtimeout":1234567,"provenance":"${
                expectedProvenance}"}`;
        }
        return new RegExp(pattern);
    }

    let pattern = `command: ${cmdName} `;
    pattern += `.* comment: "${targetId}"`;
    if (test.checkReadConcern) {
        pattern += `.* readConcern:{ level: "majority", provenance: "${expectedProvenance}" }`;
    }
    if (test.checkWriteConcern) {
        pattern += `.* writeConcern:{ w: "majority", wtimeout: 1234567, provenance: "${
            expectedProvenance}" }`;
    }
    return new RegExp(pattern);
}

// Example profile document, indicating the fields matched by the filter generated by this function:
// {
//     "op" : "insert",
//     "ns" : "test.foo",
//     "command" : {
//         "aggregate" : "foo",
//         "pipeline" : [ ... ],
//         ...
// >>>     "comment" : "5e2fd42654a611422be06518",
//         "lsid" : { ... },
//         ...
//     },
//     "keysExamined" : 0,
//     "docsExamined" : 1,
//     ...
//     "readConcern" : {
// >>>     "level" : "majority"
//     },
//     "writeConcern" : {
// >>>     "w" : "majority",
// >>>     "wtimeout" : 1234567
//     },
//     "storage" : { },
//     "responseLength" : 222,
//     "protocol" : "op_msg",
//     "millis" : 194,
//     "planSummary" : "COLLSCAN",
//      ...
// }
function createProfileFilterForTestCase(test, targetId, explicitRWC) {
    let expectedProvenance = explicitRWC ? "clientSupplied" : "customDefault";
    let commandProfile = {
        "command.comment": targetId,
        /* Filter out failed operations */
        errCode: {$exists: false}
    };
    if (test.checkReadConcern) {
        commandProfile = Object.extend(
            {"readConcern.level": "majority", "readConcern.provenance": expectedProvenance},
            commandProfile);
    }
    if (test.checkWriteConcern) {
        commandProfile = Object.extend({
            "writeConcern.w": "majority",
            "writeConcern.wtimeout": 1234567,
            "writeConcern.provenance": expectedProvenance
        },
                                       commandProfile);
    }
    return commandProfile;
}

function runScenario(
    desc, conn, regularCheckConn, configSvrCheckConn, {explicitRWC, explicitProvenance = false}) {
    let runCommandTest = function(cmdName, test) {
        assert(test !== undefined,
               "coverage failure: must define a RWC defaults application test for " + cmdName);

        if (test.skip !== undefined) {
            print("skipping " + cmdName + ": " + test.skip);
            return;
        }
        validateTestCase(test);

        let sharded = !!configSvrCheckConn;

        let thisTestDesc = desc + " (" + (sharded ? "sharded" : "non-sharded") +
            ")\ntesting command " + cmdName + " " + tojson(test.command);
        jsTest.log(thisTestDesc);

        if (test.target) {
            // If this test is only suitable for sharded or replset, skip it in the other case.
            if (sharded && test.target != "sharded") {
                return;
            }
            if (!sharded && test.target != "replset") {
                return;
            }
        }

        // Determine the appropriate connection to use for checking the command.
        let checkConn = regularCheckConn;
        if (sharded && test.shardedTargetsConfigServer) {
            checkConn = configSvrCheckConn;
        }

        // Set up profiling or logging, as appropriate.
        if (test.useLogs) {
            assert.commandWorked(checkConn.getDB(db).setLogLevel(1));
            assert.commandWorked(checkConn.adminCommand({clearLog: "global"}));
        } else {
            assert.commandWorked(checkConn.getDB(db).setProfilingLevel(2));
        }

        // Do any test-specific setup.
        if (typeof (test.setUp) === "function") {
            test.setUp(conn);
        }

        // Get the command from the test case.
        let actualCmd = (typeof (test.command) === "function")
            ? test.command(conn)
            : Object.assign({}, test.command, {});
        assert.eq("undefined", typeof (actualCmd.readConcern));
        assert.eq("undefined", typeof (actualCmd.writeConcern));

        // Add extra fields for RWC if necessary, and an identifying comment.
        // When sharded, the field order is: comment, readConcern, writeConcern.
        // Otherwise, the field order is: readConcern, writeConcern, comment.
        // This is necessary to work around truncation of long log lines in the RamLog when looking
        // for the operation's log line.
        let targetId = ObjectId().str;
        if (sharded) {
            actualCmd = Object.extend(actualCmd, {comment: targetId});
        }
        if (explicitRWC) {
            if (test.checkReadConcern) {
                let explicitRC = {level: 'majority'};
                if (explicitProvenance) {
                    explicitRC = Object.extend(explicitRC, {provenance: "clientSupplied"});
                }
                actualCmd = Object.extend(actualCmd, {readConcern: explicitRC});
            }
            if (test.checkWriteConcern) {
                let explicitWC = {w: "majority", wtimeout: 1234567};
                if (explicitProvenance) {
                    explicitWC = Object.extend(explicitWC, {provenance: "clientSupplied"});
                }
                actualCmd = Object.extend(actualCmd, {writeConcern: explicitWC});
            }
        }
        if (!sharded) {
            actualCmd = Object.extend(actualCmd, {comment: targetId});
        }

        // Run the command.
        let res = conn.getDB("db" in test ? test.db : db).runCommand(actualCmd);
        assert.commandWorked(res);

        // Check that the command applied the correct RWC.
        if (test.useLogs) {
            let re =
                createLogLineRegularExpressionForTestCase(test, cmdName, targetId, explicitRWC);
            assert(checkLog.checkContainsOnce(checkConn, re),
                   "unable to find pattern " + re + " in logs on " + checkConn + " for test " +
                       thisTestDesc);
        } else {
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: checkConn.getDB(db),
                filter: createProfileFilterForTestCase(test, targetId, explicitRWC)
            });
        }

        // Clean up the collection by dropping the DB. This also drops all associated indexes and
        // clears the profiler collection, except for users and roles.
        assert.commandWorked(checkConn.getDB(db).setLogLevel(0));
        assert.commandWorked(conn.getDB(db).runCommand({dropAllUsersFromDatabase: 1}));
        assert.commandWorked(conn.getDB(db).runCommand({dropAllRolesFromDatabase: 1}));
        assert.commandWorked(conn.getDB(db).runCommand({dropDatabase: 1}));
    };

    jsTest.log(desc);

    let res = conn.adminCommand({listCommands: 1});
    assert.commandWorked(res);

    let commands = Object.keys(res.commands);
    for (let cmdName of commands) {
        runCommandTest(cmdName, testCases[cmdName]);
    }
    // The setDefaultRWConcern entry in testCases is skip, because we run the actual case here after
    // all other commands.
    runCommandTest("setDefaultRWConcern", setDefaultRWConcernActualTestCase);
}

function runTests(conn, regularCheckConn, configSvrCheckConn) {
    // The target RWC is always {level: "majority"} and {w: "majority", wtimeout: 1234567}

    runScenario("Scenario: RWC defaults never set, explicit RWC present, absent provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: false});
    runScenario("Scenario: RWC defaults never set, explicit RWC present, explicit provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: true});

    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "majority"},
        defaultWriteConcern: {w: "majority", wtimeout: 1234567}
    }));
    runScenario("Scenario: RWC defaults set, explicit RWC absent",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: false});

    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local"},
        defaultWriteConcern: {w: 1, wtimeout: 7654321}
    }));
    runScenario("Scenario: RWC defaults set, explicit RWC present, absent provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: false});
    runScenario("Scenario: RWC defaults set, explicit RWC present, explicit provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: true});

    assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {}}));
    runScenario("Scenario: Read concern defaults unset, explicit RWC present, absent provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: false});
    runScenario("Scenario: Read concern defaults unset, explicit RWC present, explicit provenance",
                conn,
                regularCheckConn,
                configSvrCheckConn,
                {explicitRWC: true, explicitProvenance: true});
}

// TODO SERVER-45052: Move the main code into jstests/lib, and then call it from jstests/replsets
// and jstests/sharding.

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
runTests(rst.getPrimary(), rst.getPrimary(), undefined, false);
rst.stopSet();

let st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 1}}});
runTests(st.s0, st.rs0.getPrimary(), st.configRS.getPrimary(), true);
st.stop();
})();
