/**
 * Tests that the donor blocks writes that are executed while the migration in the blocking state,
 * then rejects the writes when the migration completes.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

const donorRst = new ReplSetTest(
    {nodes: 1, name: 'donor', nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: 'recipient', nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet();
donorRst.initiate();
recipientRst.startSet();
recipientRst.initiate();

const primary = donorRst.getPrimary();
const kRecipientConnString = recipientRst.getURL();
const kCollName = "testColl";

const kTenantDefinedDbName = "0";
const kTestDoc = {
    x: -1
};

const kTestIndexKey = {
    x: 1
};
const kExpireAfterSeconds = 1000000;
const kTestIndex = {
    key: kTestIndexKey,
    name: "testIndex",
    expireAfterSeconds: kExpireAfterSeconds
};

const kNumInitialDocs = 2;  // num initial docs to insert into test collections.
const kMaxSize = 1024;      // max size of capped collections.
const kTxnNumber = NumberLong(0);
const kMaxTimeMS = 1 * 1000;

function startMigration(host, dbName, recipientConnString) {
    const primary = new Mongo(host);
    const dbPrefix = dbName.split('_')[0];
    return primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: recipientConnString,
        databasePrefix: dbPrefix,
        readPreference: {mode: "primary"}
    });
}

/**
 * To be used to resume a migration that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetBlockedWrites' and unpauses the migration.
 */
function resumeMigrationAfterBlockingWrite(host, targetBlockedWrites) {
    load("jstests/libs/fail_point_util.js");
    const primary = new Mongo(host);

    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "tenantMigrationBlockWrite",
        timesEntered: targetBlockedWrites,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "pauseTenantMigrationAfterBlockingStarts", mode: "off"}));
}

function createCollectionAndInsertDocs(primaryDB, collName, isCapped, numDocs = kNumInitialDocs) {
    const createCollCommand = {create: collName};
    if (isCapped) {
        createCollCommand.capped = true;
        createCollCommand.size = kMaxSize;
    }
    assert.commandWorked(primaryDB.runCommand(createCollCommand));

    let bulk = primaryDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({x: i});
    }
    assert.commandWorked(bulk.execute());
}

function insertTestDoc(primaryDB, collName) {
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [kTestDoc]}));
}

function createTestIndex(primaryDB, collName) {
    assert.commandWorked(primaryDB.runCommand({createIndexes: collName, indexes: [kTestIndex]}));
}

function countDocs(db, collName, query) {
    const res = assert.commandWorked(db.runCommand({count: collName, query: query}));
    return res.n;
}

function databaseExists(db, dbName) {
    const res = assert.commandWorked(db.adminCommand({listDatabases: 1}));
    return res.databases.some((dbDoc => dbDoc.name === dbName));
}

function collectionExists(db, collName) {
    const res = assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    return res.cursor.firstBatch.length == 1;
}

function indexExists(db, collName, targetIndex) {
    const res = assert.commandWorked(db.runCommand({listIndexes: collName}));
    return res.cursor.firstBatch.some(
        (index) => bsonWoCompare(index.key, targetIndex.key) === 0 &&
            bsonWoCompare(index.expireAfterSeconds, targetIndex.expireAfterSeconds) === 0);
}

function validateTestCase(testCase) {
    assert(testCase.skip || testCase.command,
           "must specify exactly one of 'skip' or 'command' for test case " + tojson(testCase));

    if (testCase.skip) {
        return;
    }

    assert(testCase.command, "must specify 'command' for test case " + tojson(testCase));

    // Check that all present fields are of the correct type.
    assert(typeof (testCase.command) === "function");
    assert(typeof (testCase.assertCommandFailed) === "function");
    assert(testCase.setUp ? typeof (testCase.setUp) === "function" : true);
    assert(testCase.runAgainstAdminDb ? typeof (testCase.runAgainstAdminDb) === "boolean" : true);
    assert(testCase.explicitlyCreateCollection
               ? typeof (testCase.explicitlyCreateCollection) === "boolean"
               : true);
    assert(testCase.isSupportedInTransaction
               ? typeof (testCase.isSupportedInTransaction) === "boolean"
               : true);
    assert(testCase.isRetryableWriteCommand
               ? typeof (testCase.isRetryableWriteCommand) === "boolean"
               : true);
}

function makeTestOptions(primary, testCase, dbName, collName, useTransaction, useRetryableWrite) {
    assert(!useTransaction || !useRetryableWrite);

    const useSession = useTransaction || useRetryableWrite;
    const primaryConn = useSession ? primary.startSession({causalConsistency: false}) : primary;
    const primaryDB = useSession ? primaryConn.getDatabase(dbName) : primaryConn.getDB(dbName);

    let command = testCase.command(dbName, collName);

    if (useTransaction || useRetryableWrite) {
        command.txnNumber = kTxnNumber;
    }
    if (useTransaction) {
        command.startTransaction = true;
        command.autocommit = false;
    }

    return {
        primaryConn,
        primaryDB,
        primaryHost: useSession ? primaryConn.getClient().host : primaryConn.host,
        runAgainstAdminDb: testCase.runAgainstAdminDb,
        command,
        dbName,
        collName,
        useSession,
        useTransaction
    };
}

function runTest(
    primary, testCase, testFunc, dbName, collName, {useTransaction, useRetryableWrite} = {}) {
    const testOpts =
        makeTestOptions(primary, testCase, dbName, collName, useTransaction, useRetryableWrite);
    jsTest.log("Testing command " + tojson(testOpts.command));

    if (testCase.explicitlyCreateCollection) {
        createCollectionAndInsertDocs(testOpts.primaryDB, collName, testCase.isCapped);
    }

    if (testCase.setUp) {
        testCase.setUp(testOpts.primaryDB, collName);
    }

    testFunc(testCase, testOpts);
}

function runCommand(testOpts, expectedError) {
    let res;

    if (testOpts.useTransaction) {
        // Test that the command doesn't throw an error but the transaction cannot commit.
        testOpts.primaryConn.startTransaction();
        assert.commandWorked(testOpts.runAgainstAdminDb
                                 ? testOpts.primaryDB.adminCommand(testOpts.command)
                                 : testOpts.primaryDB.runCommand(testOpts.command));
        res = testOpts.primaryConn.commitTransaction_forTesting();
    } else {
        res = testOpts.runAgainstAdminDb ? testOpts.primaryDB.adminCommand(testOpts.command)
                                         : testOpts.primaryDB.runCommand(testOpts.command);
    }

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError);
    } else {
        assert.commandWorked(res);
    }
}

/**
 * Tests that the write succeeds when there is no migration.
 */
function testWriteNoMigration(testCase, testOpts) {
    runCommand(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

/**
 * Tests that the donor rejects writes after the migration commits.
 */
function testWriteIsRejectedIfSentAfterMigrationHasCommitted(testCase, testOpts) {
    const dbPrefix = testOpts.dbName.split('_')[0];
    assert.commandWorked(testOpts.primaryDB.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbPrefix,
        readPreference: {mode: "primary"}
    }));

    runCommand(testOpts, ErrorCodes.TenantMigrationCommitted);
    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

/**
 * Tests that the donor does not reject writes after the migration aborts.
 */
function testWriteIsAcceptedIfSentAfterMigrationHasAborted(testCase, testOpts) {
    let abortFp = configureFailPoint(testOpts.primaryDB, "abortTenantMigrationAfterBlockingStarts");
    const dbPrefix = testOpts.dbName.split('_')[0];
    assert.commandFailedWithCode(testOpts.primaryDB.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbPrefix,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.TenantMigrationAborted);
    abortFp.off();

    runCommand(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

/**
 * Tests that the donor blocks writes that are executed in the blocking state.
 */
function testWriteBlocksIfMigrationIsInBlocking(testCase, testOpts) {
    let blockingFp =
        configureFailPoint(testOpts.primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(startMigration, testOpts.primaryHost, testOpts.dbName, kRecipientConnString);

    // Run the command after the migration enters the blocking state.
    migrationThread.start();
    blockingFp.wait();
    testOpts.command.maxTimeMS = kMaxTimeMS;
    runCommand(testOpts, ErrorCodes.MaxTimeMSExpired);

    // Allow the migration to complete.
    blockingFp.off();
    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());

    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

/**
 * Tests that the donor blocks writes that are executed in the blocking state and rejects them after
 * the migration commits.
 */
function testBlockedWriteGetsUnblockedAndRejectedIfMigrationCommits(testCase, testOpts) {
    let blockingFp =
        configureFailPoint(testOpts.primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const targetBlockedWrites =
        assert
            .commandWorked(testOpts.primaryDB.adminCommand(
                {configureFailPoint: "tenantMigrationBlockWrite", mode: "alwaysOn"}))
            .count +
        1;

    let migrationThread =
        new Thread(startMigration, testOpts.primaryHost, testOpts.dbName, kRecipientConnString);
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingWrite, testOpts.primaryHost, targetBlockedWrites);

    // Run the command after the migration enters the blocking state.
    resumeMigrationThread.start();
    migrationThread.start();
    blockingFp.wait();

    // The migration should unpause and commit after the write is blocked. Verify that the write is
    // rejected.
    runCommand(testOpts, ErrorCodes.TenantMigrationCommitted);

    // Verify that the migration succeeded.
    resumeMigrationThread.join();
    migrationThread.join();
    assert.commandWorked(migrationThread.returnData());

    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

/**
 * Tests that the donor blocks writes that are executed in the blocking state and rejects them after
 * the migration aborts.
 */
function testBlockedReadGetsUnblockedAndRejectedIfMigrationAborts(testCase, testOpts) {
    let blockingFp =
        configureFailPoint(testOpts.primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let abortFp = configureFailPoint(testOpts.primaryDB, "abortTenantMigrationAfterBlockingStarts");
    const targetBlockedWrites =
        assert
            .commandWorked(testOpts.primaryDB.adminCommand(
                {configureFailPoint: "tenantMigrationBlockWrite", mode: "alwaysOn"}))
            .count +
        1;

    let migrationThread =
        new Thread(startMigration, testOpts.primaryHost, testOpts.dbName, kRecipientConnString);
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingWrite, testOpts.primaryHost, targetBlockedWrites);

    // Run the command after the migration enters the blocking state.
    resumeMigrationThread.start();
    migrationThread.start();
    blockingFp.wait();

    // The migration should unpause and abort after the write is blocked. Verify that the write is
    // rejected.
    runCommand(testOpts, ErrorCodes.TenantMigrationAborted);

    // Verify that the migration aborted due to the simulated error.
    resumeMigrationThread.join();
    migrationThread.join();
    abortFp.off();
    assert.commandFailedWithCode(migrationThread.returnData(), ErrorCodes.TenantMigrationAborted);

    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

const isNotWriteCommand = "not a write command";
const isNotRunOnUserDatabase = "not run on user database";
const isAuthCommand = "is an auth command";
const isOnlySupportedOnStandalone = "is only supported on standalone";
const isOnlySupportedOnShardedCluster = "is only supported on sharded cluster";
const isDeprecated = "is only deprecated";

const testCases = {
    _addShard: {skip: isNotRunOnUserDatabase},
    _cloneCollectionOptionsFromPrimaryShard: {skip: isNotRunOnUserDatabase},
    _configsvrAddShard: {skip: isNotRunOnUserDatabase},
    _configsvrAddShardToZone: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerCollectionStatus: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStart: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStatus: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStop: {skip: isNotRunOnUserDatabase},
    _configsvrClearJumboFlag: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunkMerge: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunkMigration: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunkSplit: {skip: isNotRunOnUserDatabase},
    _configsvrCommitMovePrimary: {skip: isNotRunOnUserDatabase},
    _configsvrCreateDatabase: {skip: isNotRunOnUserDatabase},
    _configsvrDropCollection: {skip: isNotRunOnUserDatabase},
    _configsvrDropDatabase: {skip: isNotRunOnUserDatabase},
    _configsvrEnableSharding: {skip: isNotRunOnUserDatabase},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isNotRunOnUserDatabase},
    _configsvrMoveChunk: {skip: isNotRunOnUserDatabase},
    _configsvrMovePrimary: {skip: isNotRunOnUserDatabase},
    _configsvrRefineCollectionShardKey: {skip: isNotRunOnUserDatabase},
    _configsvrRemoveShard: {skip: isNotRunOnUserDatabase},
    _configsvrRemoveShardFromZone: {skip: isNotRunOnUserDatabase},
    _configsvrShardCollection: {skip: isNotRunOnUserDatabase},
    _configsvrUpdateZoneKeyRange: {skip: isNotRunOnUserDatabase},
    _flushDatabaseCacheUpdates: {skip: isNotRunOnUserDatabase},
    _flushRoutingTableCacheUpdates: {skip: isNotRunOnUserDatabase},
    _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: isNotRunOnUserDatabase},
    _getNextSessionMods: {skip: isNotRunOnUserDatabase},
    _getUserCacheGeneration: {skip: isNotRunOnUserDatabase},
    _hashBSONElement: {skip: isNotRunOnUserDatabase},
    _isSelf: {skip: isNotRunOnUserDatabase},
    _killOperations: {skip: isNotRunOnUserDatabase},
    _mergeAuthzCollections: {skip: isNotRunOnUserDatabase},
    _migrateClone: {skip: isNotRunOnUserDatabase},
    _recvChunkAbort: {skip: isNotRunOnUserDatabase},
    _recvChunkCommit: {skip: isNotRunOnUserDatabase},
    _recvChunkStart: {skip: isNotRunOnUserDatabase},
    _recvChunkStatus: {skip: isNotRunOnUserDatabase},
    _shardsvrCloneCatalogData: {skip: isNotRunOnUserDatabase},
    _shardsvrMovePrimary: {skip: isNotRunOnUserDatabase},
    _shardsvrShardCollection: {skip: isNotRunOnUserDatabase},
    _transferMods: {skip: isNotRunOnUserDatabase},
    abortTransaction: {
        skip: true,  // TODO (SERVER-49844)
    },
    aggregate: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {
                aggregate: collName,
                pipeline: [{$out: collName + "Out"}],
                cursor: {batchSize: 1}
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(collectionExists(db, collName + "Out"));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!collectionExists(db, collName + "Out"));
        }
    },
    appendOplogNote: {skip: isNotRunOnUserDatabase},
    applyOps: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {applyOps: [{op: "i", ns: dbName + "." + collName, o: {_id: 0}}]};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, {_id: 0}), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, {_id: 0}), 0);
        }
    },
    authenticate: {skip: isAuthCommand},
    availableQueryOptions: {skip: isNotWriteCommand},
    buildInfo: {skip: isNotWriteCommand},
    captrunc: {
        skip: isNotWriteCommand,           // TODO (SERVER-49834)
        explicitlyCreateCollection: true,  // creates a collection with kNumInitialDocs > 1 docs.
        isCapped: true,
        command: function(dbName, collName) {
            return {captrunc: collName, n: 1};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, {}), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, {}), kNumInitialDocs);
        }
    },
    checkShardingIndex: {skip: isNotRunOnUserDatabase},
    cleanupOrphaned: {skip: isNotRunOnUserDatabase},
    clearLog: {skip: isNotRunOnUserDatabase},
    cloneCollectionAsCapped: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {
                cloneCollectionAsCapped: collName,
                toCollection: collName + "CloneCollectionAsCapped",
                size: kMaxSize
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(collectionExists(db, collName + "CloneCollectionAsCapped"));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!collectionExists(db, collName + "CloneCollectionAsCapped"));
        }
    },
    collMod: {
        explicitlyCreateCollection: true,
        setUp: createTestIndex,
        command: function(dbName, collName) {
            return {
                collMod: collName,
                index: {keyPattern: kTestIndexKey, expireAfterSeconds: kExpireAfterSeconds + 1}
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(indexExists(
                db, collName, {key: kTestIndexKey, expireAfterSeconds: kExpireAfterSeconds + 1}));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!indexExists(
                db, collName, {key: kTestIndexKey, expireAfterSeconds: kExpireAfterSeconds + 1}));
        }
    },
    collStats: {skip: isNotWriteCommand},
    commitTransaction: {
        skip: true,  // TODO (SERVER-49844)
    },
    compact: {
        skip: isNotWriteCommand,  // TODO (SERVER-49834)
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {compact: collName, force: true};
        },
        assertCommandSucceeded: function(db, dbName, collName) {},
        assertCommandFailed: function(db, dbName, collName) {}
    },
    configureFailPoint: {skip: isNotRunOnUserDatabase},
    connPoolStats: {skip: isNotRunOnUserDatabase},
    connPoolSync: {skip: isNotRunOnUserDatabase},
    connectionStatus: {skip: isNotRunOnUserDatabase},
    convertToCapped: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {convertToCapped: collName, size: kMaxSize};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(db[collName].stats().capped);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!db[collName].stats().capped);
        }
    },
    coordinateCommitTransaction: {skip: isNotRunOnUserDatabase},
    count: {skip: isNotWriteCommand},
    cpuload: {skip: isNotRunOnUserDatabase},
    create: {
        command: function(dbName, collName) {
            return {create: collName};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(collectionExists(db, collName));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!collectionExists(db, collName));
        }
    },
    createIndexes: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {createIndexes: collName, indexes: [kTestIndex]};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(indexExists(db, collName, kTestIndex));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!indexExists(db, collName, kTestIndex));
        }
    },
    createRole: {skip: isAuthCommand},
    createUser: {skip: isAuthCommand},
    currentOp: {skip: isNotRunOnUserDatabase},
    dataSize: {skip: isNotWriteCommand},
    dbCheck: {skip: isNotWriteCommand},
    dbHash: {skip: isNotWriteCommand},
    dbStats: {skip: isNotWriteCommand},
    delete: {
        isSupportedInTransaction: true,
        isRetryableWriteCommand: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {delete: collName, deletes: [{q: kTestDoc, limit: 1}]};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 1);
        }
    },
    distinct: {skip: isNotWriteCommand},
    donorForgetMigration: {skip: isNotRunOnUserDatabase},
    donorStartMigration: {skip: isNotRunOnUserDatabase},
    donorWaitForMigrationToCommit: {skip: isNotRunOnUserDatabase},
    driverOIDTest: {skip: isNotRunOnUserDatabase},
    drop: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {drop: collName};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(!collectionExists(db, collName));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(collectionExists(db, collName));
        }
    },
    dropAllRolesFromDatabase: {skip: isAuthCommand},
    dropAllUsersFromDatabase: {skip: isAuthCommand},
    dropConnections: {skip: isNotRunOnUserDatabase},
    dropDatabase: {
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {dropDatabase: 1};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(!databaseExists(db, dbName));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(databaseExists(db, dbName));
        }
    },
    dropIndexes: {
        explicitlyCreateCollection: true,
        setUp: createTestIndex,
        command: function(dbName, collName) {
            return {dropIndexes: collName, index: "*"};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(!indexExists(db, collName, kTestIndex));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(indexExists(db, collName, kTestIndex));
        }
    },
    dropRole: {skip: isAuthCommand},
    dropUser: {skip: isAuthCommand},
    echo: {skip: isNotRunOnUserDatabase},
    emptycapped: {
        explicitlyCreateCollection: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {emptycapped: collName};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 1);
        }
    },
    endSessions: {skip: isNotRunOnUserDatabase},
    explain: {skip: isNotRunOnUserDatabase},
    features: {skip: isNotRunOnUserDatabase},
    filemd5: {skip: isNotWriteCommand},
    find: {skip: isNotWriteCommand},
    findAndModify: {
        isSupportedInTransaction: true,
        isRetryableWriteCommand: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {findAndModify: collName, query: kTestDoc, remove: true};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 1);
        }
    },
    flushRouterConfig: {skip: isNotRunOnUserDatabase},
    fsync: {skip: isNotRunOnUserDatabase},
    fsyncUnlock: {skip: isNotRunOnUserDatabase},
    geoSearch: {skip: isNotWriteCommand},
    getCmdLineOpts: {skip: isNotRunOnUserDatabase},
    getDatabaseVersion: {skip: isNotRunOnUserDatabase},
    getDefaultRWConcern: {skip: isNotRunOnUserDatabase},
    getDiagnosticData: {skip: isNotRunOnUserDatabase},
    getFreeMonitoringStatus: {skip: isNotRunOnUserDatabase},
    getLastError: {skip: isNotWriteCommand},
    getLog: {skip: isNotRunOnUserDatabase},
    getMore: {skip: isNotWriteCommand},
    getParameter: {skip: isNotRunOnUserDatabase},
    getShardMap: {skip: isNotRunOnUserDatabase},
    getShardVersion: {skip: isNotRunOnUserDatabase},
    getnonce: {skip: isNotRunOnUserDatabase},
    godinsert: {skip: isNotRunOnUserDatabase},
    grantPrivilegesToRole: {skip: isAuthCommand},
    grantRolesToRole: {skip: isAuthCommand},
    grantRolesToUser: {skip: isAuthCommand},
    hello: {skip: isNotRunOnUserDatabase},
    hostInfo: {skip: isNotRunOnUserDatabase},
    httpClientRequest: {skip: isNotRunOnUserDatabase},
    insert: {
        isSupportedInTransaction: true,
        isRetryableWriteCommand: true,
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {insert: collName, documents: [kTestDoc]};
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, kTestDoc), 0);
        }
    },
    internalRenameIfOptionsAndIndexesMatch: {skip: isNotRunOnUserDatabase},
    invalidateUserCache: {skip: isNotRunOnUserDatabase},
    killAllSessions: {skip: isNotRunOnUserDatabase},
    killAllSessionsByPattern: {skip: isNotRunOnUserDatabase},
    killCursors: {skip: isNotWriteCommand},
    killOp: {skip: isNotRunOnUserDatabase},
    killSessions: {skip: isNotRunOnUserDatabase},
    listCollections: {skip: isNotRunOnUserDatabase},
    listCommands: {skip: isNotRunOnUserDatabase},
    listDatabases: {skip: isNotRunOnUserDatabase},
    listIndexes: {skip: isNotWriteCommand},
    lockInfo: {skip: isNotRunOnUserDatabase},
    logRotate: {skip: isNotRunOnUserDatabase},
    logout: {skip: isNotRunOnUserDatabase},
    makeSnapshot: {skip: isNotRunOnUserDatabase},
    mapReduce: {
        command: function(dbName, collName) {
            return {
                mapReduce: collName,
                map: function mapFunc() {
                    emit(this.x, 1);
                },
                reduce: function reduceFunc(key, values) {
                    return Array.sum(values);
                },
                out: {replace: collName + "MrOut"},
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(collectionExists(db, collName + "MrOut"));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!collectionExists(db, collName + "MrOut"));
        }
    },
    mergeChunks: {skip: isNotRunOnUserDatabase},
    moveChunk: {skip: isNotRunOnUserDatabase},
    ping: {skip: isNotRunOnUserDatabase},
    planCacheClear: {skip: isNotWriteCommand},
    planCacheClearFilters: {skip: isNotWriteCommand},
    planCacheListFilters: {skip: isNotWriteCommand},
    planCacheSetFilter: {skip: isNotWriteCommand},
    prepareTransaction: {skip: isOnlySupportedOnShardedCluster},
    profile: {skip: isNotRunOnUserDatabase},
    reIndex: {skip: isOnlySupportedOnStandalone},
    reapLogicalSessionCacheNow: {skip: isNotRunOnUserDatabase},
    refreshLogicalSessionCacheNow: {skip: isNotRunOnUserDatabase},
    refreshSessions: {skip: isNotRunOnUserDatabase},
    renameCollection: {
        runAgainstAdminDb: true,
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {
                renameCollection: dbName + "." + collName,
                to: dbName + "." + collName + "Renamed"
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(!collectionExists(db, collName));
            assert(collectionExists(db, collName + "Renamed"));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(collectionExists(db, collName));
            assert(!collectionExists(db, collName + "Renamed"));
        }
    },
    repairDatabase: {skip: isDeprecated},
    replSetAbortPrimaryCatchUp: {skip: isNotRunOnUserDatabase},
    replSetFreeze: {skip: isNotRunOnUserDatabase},
    replSetGetConfig: {skip: isNotRunOnUserDatabase},
    replSetGetRBID: {skip: isNotRunOnUserDatabase},
    replSetGetStatus: {skip: isNotRunOnUserDatabase},
    replSetHeartbeat: {skip: isNotRunOnUserDatabase},
    replSetInitiate: {skip: isNotRunOnUserDatabase},
    replSetMaintenance: {skip: isNotRunOnUserDatabase},
    replSetReconfig: {skip: isNotRunOnUserDatabase},
    replSetRequestVotes: {skip: isNotRunOnUserDatabase},
    replSetResizeOplog: {skip: isNotRunOnUserDatabase},
    replSetStepDown: {skip: isNotRunOnUserDatabase},
    replSetStepUp: {skip: isNotRunOnUserDatabase},
    replSetSyncFrom: {skip: isNotRunOnUserDatabase},
    replSetTest: {skip: isNotRunOnUserDatabase},
    replSetTestEgress: {skip: isNotRunOnUserDatabase},
    replSetUpdatePosition: {skip: isNotRunOnUserDatabase},
    resetError: {skip: isNotRunOnUserDatabase},
    revokePrivilegesFromRole: {skip: isAuthCommand},
    revokeRolesFromRole: {skip: isAuthCommand},
    revokeRolesFromUser: {skip: isAuthCommand},
    rolesInfo: {skip: isNotWriteCommand},
    rotateCertificates: {skip: isAuthCommand},
    saslContinue: {skip: isAuthCommand},
    saslStart: {skip: isAuthCommand},
    sbe: {skip: isNotRunOnUserDatabase},
    serverStatus: {skip: isNotRunOnUserDatabase},
    setCommittedSnapshot: {skip: isNotRunOnUserDatabase},
    setDefaultRWConcern: {skip: isNotRunOnUserDatabase},
    setFeatureCompatibilityVersion: {skip: isNotRunOnUserDatabase},
    setFreeMonitoring: {skip: isNotRunOnUserDatabase},
    setIndexCommitQuorum: {skip: isNotRunOnUserDatabase},
    setParameter: {skip: isNotRunOnUserDatabase},
    setShardVersion: {skip: isNotRunOnUserDatabase},
    shardConnPoolStats: {skip: isNotRunOnUserDatabase},
    shardingState: {skip: isNotRunOnUserDatabase},
    shutdown: {skip: isNotRunOnUserDatabase},
    sleep: {skip: isNotRunOnUserDatabase},
    splitChunk: {skip: isNotRunOnUserDatabase},
    splitVector: {skip: isNotRunOnUserDatabase},
    stageDebug: {skip: isNotRunOnUserDatabase},
    startRecordingTraffic: {skip: isNotRunOnUserDatabase},
    startSession: {skip: isNotRunOnUserDatabase},
    stopRecordingTraffic: {skip: isNotRunOnUserDatabase},
    top: {skip: isNotRunOnUserDatabase},
    unsetSharding: {skip: isNotRunOnUserDatabase},
    update: {
        isSupportedInTransaction: true,
        isRetryableWriteCommand: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {
                update: collName,
                updates: [{q: kTestDoc, u: {$set: {y: 0}}, upsert: false, multi: false}]
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, Object.assign({y: 0}, kTestDoc)), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, Object.assign({y: 0}, kTestDoc)), 0);
        }
    },
    updateRole: {skip: isAuthCommand},
    updateUser: {skip: isNotRunOnUserDatabase},
    usersInfo: {skip: isNotRunOnUserDatabase},
    validate: {skip: isNotWriteCommand},
    voteCommitIndexBuild: {skip: isNotRunOnUserDatabase},
    waitForFailPoint: {skip: isNotRunOnUserDatabase},
    waitForOngoingChunkSplits: {skip: isNotRunOnUserDatabase},
    whatsmysni: {skip: isNotRunOnUserDatabase},
    whatsmyuri: {skip: isNotRunOnUserDatabase}
};

// Validate test cases for all commands.
for (let command of Object.keys(testCases)) {
    validateTestCase(testCases[command]);
}

// Run test cases.
const testFuncs = {
    noMigration: testWriteNoMigration,  // verify that the test cases are correct.
    inCommitted: testWriteIsRejectedIfSentAfterMigrationHasCommitted,
    inAborted: testWriteIsAcceptedIfSentAfterMigrationHasAborted,
    inBlocking: testWriteBlocksIfMigrationIsInBlocking,
    inBlockingThenCommitted: testBlockedWriteGetsUnblockedAndRejectedIfMigrationCommits,
    inBlockingThenAborted: testBlockedReadGetsUnblockedAndRejectedIfMigrationAborts
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        // Database name is [tenant_id (database prefix)]_[tenant defined database name]
        let baseDbName = commandName + "-" + testName + "0";

        if (testCase.skip) {
            print("Skipping " + commandName + ": " + testCase.skip);
            continue;
        }

        runTest(primary,
                testCase,
                testFunc,
                baseDbName + "Basic" +
                    "_" + kTenantDefinedDbName,
                kCollName);

        // TODO (SERVER-49844): Test transactional writes during migration.
        if (testCase.isSupportedInTransaction && testName == "noMigration") {
            runTest(
                primary, testCase, testFunc, baseDbName + "Txn", kCollName, {useTransaction: true});
        }

        if (testCase.isRetryableWriteCommand) {
            runTest(primary,
                    testCase,
                    testFunc,
                    baseDbName + "Retryable" +
                        "_" + kTenantDefinedDbName,
                    kCollName,
                    {useRetryableWrite: true});
        }
    }
}

donorRst.stopSet();
recipientRst.stopSet();
})();
