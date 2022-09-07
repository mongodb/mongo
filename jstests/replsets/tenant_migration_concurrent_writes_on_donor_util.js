/**
 * This utility file is used to list the different test cases needed for the
 * tenant_migration_concurrent_writes_on_donor*tests.
 */

'use strict';

var TenantMigrationConcurrentWriteUtil = (function() {});

/**
 * Asserts that the TenantMigrationAccessBlocker for the given tenant on the given node has the
 * expected statistics.
 */
function checkTenantMigrationAccessBlockerForConcurrentWritesTest(node, tenantId, {
    numBlockedWrites = 0,
    numTenantMigrationCommittedErrors = 0,
    numTenantMigrationAbortedErrors = 0
}) {
    const mtab =
        TenantMigrationUtil.getTenantMigrationAccessBlocker({donorNode: node, tenantId}).donor;
    if (!mtab) {
        assert.eq(0, numBlockedWrites);
        assert.eq(0, numTenantMigrationCommittedErrors);
        assert.eq(0, numTenantMigrationAbortedErrors);
        return;
    }

    assert.eq(mtab.numBlockedReads, 0, tojson(mtab));
    assert.eq(mtab.numBlockedWrites, numBlockedWrites, tojson(mtab));
    assert.eq(
        mtab.numTenantMigrationCommittedErrors, numTenantMigrationCommittedErrors, tojson(mtab));
    assert.eq(mtab.numTenantMigrationAbortedErrors, numTenantMigrationAbortedErrors, tojson(mtab));
}

function runCommandForConcurrentWritesTest(testOpts, expectedError) {
    let res;
    if (testOpts.isMultiUpdate && !testOpts.testInTransaction) {
        // Multi writes outside a transaction cannot be automatically retried, so we return a
        // different error code than usual. This does not apply to the MaxTimeMS case because the
        // error in that case is already not retryable.
        if (expectedError == ErrorCodes.TenantMigrationCommitted ||
            expectedError == ErrorCodes.TenantMigrationAborted) {
            expectedError = ErrorCodes.Interrupted;
        }
    }

    if (testOpts.testInTransaction) {
        // Since oplog entries for write commands inside a transaction are not generated until the
        // commitTransaction command is run, here we assert on the response of the commitTransaction
        // command instead.
        assert.commandWorked(testOpts.runAgainstAdminDb
                                 ? testOpts.primaryDB.adminCommand(testOpts.command)
                                 : testOpts.primaryDB.runCommand(testOpts.command));
        let commitTxnCommand = {
            commitTransaction: 1,
            txnNumber: testOpts.command.txnNumber,
            autocommit: false,
            writeConcern: {w: "majority"}
        };

        // 'testBlockWritesAfterMigrationEnteredBlocking' runs each write command with maxTimeMS
        // attached and asserts that the command blocks and fails with MaxTimeMSExpired. So in the
        // case of transactions, we want to assert that commitTransaction blocks and fails
        // MaxTimeMSExpired instead.
        if (testOpts.command.maxTimeMS) {
            commitTxnCommand.maxTimeMS = testOpts.command.maxTimeMS;
        }

        res = testOpts.primaryDB.adminCommand(commitTxnCommand);
    } else {
        res = testOpts.runAgainstAdminDb ? testOpts.primaryDB.adminCommand(testOpts.command)
                                         : testOpts.primaryDB.runCommand(testOpts.command);
    }

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError);

        const expectTransientTransactionError = testOpts.testInTransaction &&
            (expectedError == ErrorCodes.TenantMigrationAborted ||
             expectedError == ErrorCodes.TenantMigrationCommitted);
        if (expectTransientTransactionError) {
            assert(res["errorLabels"] != null, "Error labels are absent from " + tojson(res));
            const expectedErrorLabels = ['TransientTransactionError'];
            assert.sameMembers(res["errorLabels"],
                               expectedErrorLabels,
                               "Error labels " + tojson(res["errorLabels"]) +
                                   " are different from expected " + expectedErrorLabels);
        }

        const expectTopLevelError = !testOpts.isBatchWrite ||
            ErrorCodes.isInterruption(expectedError) || expectTransientTransactionError;
        if (expectTopLevelError) {
            assert.eq(res.code, expectedError, tojson(res));
            assert.eq(res.ok, 0, tojson(res));
        } else {
            assert.isnull(res.code, tojson(res));
            assert.eq(res.ok, 1, tojson(res));
        }
    } else {
        assert.commandWorked(res);
    }
}

function createCollectionAndInsertDocsForConcurrentWritesTest(
    primaryDB, collName, isCapped, numDocs = TenantMigrationConcurrentWriteUtil.kNumInitialDocs) {
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

function cleanUpForConcurrentWritesTest(dbName, donorPrimary) {
    // To avoid disk space errors, ensure a new snapshot after dropping the DB,
    // so subsequent 'Shard Merge' migrations don't copy it again.
    const donorDB = donorPrimary.getDB(dbName);
    assert.commandWorked(donorDB.dropDatabase());
}

function makeTestOptionsForConcurrentWritesTest(
    primary, testCase, dbName, collName, testInTransaction, testAsRetryableWrite) {
    assert(!testInTransaction || !testAsRetryableWrite);

    const useSession = testInTransaction || testAsRetryableWrite || testCase.isTransactionCommand;
    const primaryConn = useSession ? primary.startSession({causalConsistency: false}) : primary;
    const primaryDB = useSession ? primaryConn.getDatabase(dbName) : primaryConn.getDB(dbName);

    let command = testCase.command(dbName, collName);

    if (testInTransaction || testAsRetryableWrite) {
        command.txnNumber = TenantMigrationConcurrentWriteUtil.kTxnNumber;
    }
    if (testInTransaction) {
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
        testInTransaction,
        isBatchWrite: testCase.isBatchWrite,
        isMultiUpdate: testCase.isMultiUpdate
    };
}

function runTestForConcurrentWritesTest(
    primary, testCase, testFunc, dbName, collName, {testInTransaction, testAsRetryableWrite} = {}) {
    const testOpts = makeTestOptionsForConcurrentWritesTest(
        primary, testCase, dbName, collName, testInTransaction, testAsRetryableWrite);
    jsTest.log("Testing testOpts: " + tojson(testOpts) + " with testFunc " + testFunc.name);

    if (testCase.explicitlyCreateCollection) {
        createCollectionAndInsertDocsForConcurrentWritesTest(
            testOpts.primaryDB, collName, testCase.isCapped);
    }

    if (testCase.setUp) {
        testCase.setUp(testOpts.primaryDB, collName, testInTransaction);
    }

    testFunc(testCase, testOpts);

    // This cleanup step is necessary for the shard merge protocol to work correctly.
    cleanUpForConcurrentWritesTest(dbName, primary);
}

function setupTestForConcurrentWritesTest(testCase, collName, testOpts) {
    if (testCase.explicitlyCreateCollection) {
        createCollectionAndInsertDocsForConcurrentWritesTest(
            testOpts.primaryDB, collName, testCase.isCapped);
    }

    if (testCase.setUp) {
        testCase.setUp(testOpts.primaryDB, collName, testOpts.testInTransaction);
    }
}

const isNotWriteCommand = "not a write command";
const isNotRunOnUserDatabase = "not run on user database";
const isNotSupportedInServerless = "not supported in serverless cluster";
const isAuthCommand = "is an auth command";
const isOnlySupportedOnStandalone = "is only supported on standalone";
const isOnlySupportedOnShardedCluster = "is only supported on sharded cluster";
const isDeprecated = "is only deprecated";

TenantMigrationConcurrentWriteUtil.kTestDoc = {
    x: -1
};
TenantMigrationConcurrentWriteUtil.kTestDoc2 = {
    x: -2
};

TenantMigrationConcurrentWriteUtil.kTestIndexKey = {
    x: 1
};
TenantMigrationConcurrentWriteUtil.kExpireAfterSeconds = 1000000;
TenantMigrationConcurrentWriteUtil.kTestIndex = {
    key: TenantMigrationConcurrentWriteUtil.kTestIndexKey,
    name: "testIndex",
    expireAfterSeconds: TenantMigrationConcurrentWriteUtil.kExpireAfterSeconds
};

function collectionExists(db, collName) {
    const res = assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
    return res.cursor.firstBatch.length == 1;
}

function insertTestDoc(primaryDB, collName) {
    assert.commandWorked(primaryDB.runCommand(
        {insert: collName, documents: [TenantMigrationConcurrentWriteUtil.kTestDoc]}));
}

function insertTwoTestDocs(primaryDB, collName) {
    assert.commandWorked(primaryDB.runCommand({
        insert: collName,
        documents: [
            TenantMigrationConcurrentWriteUtil.kTestDoc,
            TenantMigrationConcurrentWriteUtil.kTestDoc2
        ]
    }));
}

function createTestIndex(primaryDB, collName) {
    assert.commandWorked(primaryDB.runCommand(
        {createIndexes: collName, indexes: [TenantMigrationConcurrentWriteUtil.kTestIndex]}));
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

TenantMigrationConcurrentWriteUtil.kMaxSize = 1024;  // max size of capped collections.
TenantMigrationConcurrentWriteUtil.kNumInitialDocs =
    2;  // num initial docs to insert into test collections.
TenantMigrationConcurrentWriteUtil.kTxnNumber = NumberLong(0);

TenantMigrationConcurrentWriteUtil.testCases = {
    _addShard: {skip: isNotRunOnUserDatabase},
    _cloneCollectionOptionsFromPrimaryShard: {skip: isNotRunOnUserDatabase},
    _configsvrAddShard: {skip: isNotRunOnUserDatabase},
    _configsvrAddShardToZone: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerCollectionStatus: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStart: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStatus: {skip: isNotRunOnUserDatabase},
    _configsvrBalancerStop: {skip: isNotRunOnUserDatabase},
    _configsvrClearJumboFlag: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunksMerge: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunkMigration: {skip: isNotRunOnUserDatabase},
    _configsvrCommitChunkSplit: {skip: isNotRunOnUserDatabase},
    _configsvrCommitIndex: {skip: isNotRunOnUserDatabase},
    _configsvrCommitMovePrimary:
        {skip: isNotRunOnUserDatabase},  // Can be removed once 6.0 is last LTS
    _configsvrCreateDatabase: {skip: isNotRunOnUserDatabase},
    _configsvrDropIndexCatalogEntry: {skip: isNotRunOnUserDatabase},
    _configsvrEnsureChunkVersionIsGreaterThan: {skip: isNotRunOnUserDatabase},
    _configsvrMoveChunk: {skip: isNotRunOnUserDatabase},  // Can be removed once 6.0 is last LTS
    _configsvrMovePrimary: {skip: isNotRunOnUserDatabase},
    _configsvrMoveRange: {skip: isNotRunOnUserDatabase},
    _configsvrRefineCollectionShardKey: {skip: isNotRunOnUserDatabase},
    _configsvrRemoveShard: {skip: isNotRunOnUserDatabase},
    _configsvrRemoveShardFromZone: {skip: isNotRunOnUserDatabase},
    _configsvrUpdateZoneKeyRange: {skip: isNotRunOnUserDatabase},
    _flushDatabaseCacheUpdates: {skip: isNotRunOnUserDatabase},
    _flushDatabaseCacheUpdatesWithWriteConcern: {skip: isNotRunOnUserDatabase},
    _flushReshardingStateChange: {skip: isNotRunOnUserDatabase},
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
    _recvChunkReleaseCritSec: {skip: isNotRunOnUserDatabase},
    _recvChunkStart: {skip: isNotRunOnUserDatabase},
    _recvChunkStatus: {skip: isNotRunOnUserDatabase},
    _shardsvrCloneCatalogData: {skip: isNotRunOnUserDatabase},
    _shardsvrCommitIndexParticipant: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrCompactStructuredEncryptionData: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrCreateCollection: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrCreateCollectionParticipant: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrRegisterIndex: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrDropIndexCatalogEntryParticipant: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrMovePrimary: {skip: isNotRunOnUserDatabase},
    _shardsvrSetAllowMigrations: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrRenameCollection: {skip: isOnlySupportedOnShardedCluster},
    _shardsvrUnregisterIndex: {skip: isOnlySupportedOnShardedCluster},
    _transferMods: {skip: isNotRunOnUserDatabase},
    abortTransaction: {
        skip: isNotWriteCommand  // aborting unprepared transaction doesn't create an abort oplog
                                 // entry.
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
    applyOps: {skip: isNotSupportedInServerless},
    authenticate: {skip: isAuthCommand},
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
                size: TenantMigrationConcurrentWriteUtil.kMaxSize
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
                index: {
                    keyPattern: TenantMigrationConcurrentWriteUtil.kTestIndexKey,
                    expireAfterSeconds: TenantMigrationConcurrentWriteUtil.kExpireAfterSeconds + 1
                }
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(indexExists(db, collName, {
                key: TenantMigrationConcurrentWriteUtil.kTestIndexKey,
                expireAfterSeconds: TenantMigrationConcurrentWriteUtil.kExpireAfterSeconds + 1
            }));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!indexExists(db, collName, {
                key: TenantMigrationConcurrentWriteUtil.kTestIndexKey,
                expireAfterSeconds: TenantMigrationConcurrentWriteUtil.kExpireAfterSeconds + 1
            }));
        }
    },
    collStats: {skip: isNotWriteCommand},
    commitTransaction: {
        isTransactionCommand: true,
        runAgainstAdminDb: true,
        setUp: function(primaryDB, collName) {
            assert.commandWorked(primaryDB.runCommand({
                insert: collName,
                documents: [TenantMigrationConcurrentWriteUtil.kTestDoc],
                txnNumber: NumberLong(TenantMigrationConcurrentWriteUtil.kTxnNumber),
                startTransaction: true,
                autocommit: false
            }));
        },
        command: function(dbName, collName) {
            return {
                commitTransaction: 1,
                txnNumber: NumberLong(TenantMigrationConcurrentWriteUtil.kTxnNumber),
                autocommit: false,
                writeConcern: {w: "majority"}
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName), 0);
        }
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
            return {convertToCapped: collName, size: TenantMigrationConcurrentWriteUtil.kMaxSize};
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
        testInTransaction: true,
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
        testInTransaction: true,
        explicitlyCreateCollection: true,
        setUp: function(primaryDB, collName, testInTransaction) {
            if (testInTransaction) {
                // Drop the collection that was explicitly created above since inside transactions
                // the index to create must either be on a non-existing collection, or on a new
                // empty collection created earlier in the same transaction.
                assert.commandWorked(primaryDB.runCommand({drop: collName}));
            }
        },
        command: function(dbName, collName) {
            return {
                createIndexes: collName,
                indexes: [TenantMigrationConcurrentWriteUtil.kTestIndex]
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert(indexExists(db, collName, TenantMigrationConcurrentWriteUtil.kTestIndex));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(!collectionExists(db, collName) ||
                   !indexExists(db, collName, TenantMigrationConcurrentWriteUtil.kTestIndex));
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
        testInTransaction: true,
        testAsRetryableWrite: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {
                delete: collName,
                deletes: [{q: TenantMigrationConcurrentWriteUtil.kTestDoc, limit: 1}]
            };
        },
        isBatchWrite: true,
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 1);
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
            assert(!indexExists(db, collName, TenantMigrationConcurrentWriteUtil.kTestIndex));
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert(indexExists(db, collName, TenantMigrationConcurrentWriteUtil.kTestIndex));
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
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 1);
        }
    },
    endSessions: {skip: isNotRunOnUserDatabase},
    explain: {skip: isNotRunOnUserDatabase},
    features: {skip: isNotRunOnUserDatabase},
    filemd5: {skip: isNotWriteCommand},
    find: {skip: isNotWriteCommand},
    findAndModify: {
        testInTransaction: true,
        testAsRetryableWrite: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {
                findAndModify: collName,
                query: TenantMigrationConcurrentWriteUtil.kTestDoc,
                remove: true
            };
        },
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 0);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 1);
        }
    },
    flushRouterConfig: {skip: isNotRunOnUserDatabase},
    fsync: {skip: isNotRunOnUserDatabase},
    fsyncUnlock: {skip: isNotRunOnUserDatabase},
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
        testInTransaction: true,
        testAsRetryableWrite: true,
        explicitlyCreateCollection: true,
        command: function(dbName, collName) {
            return {insert: collName, documents: [TenantMigrationConcurrentWriteUtil.kTestDoc]};
        },
        isBatchWrite: true,
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, TenantMigrationConcurrentWriteUtil.kTestDoc), 0);
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
    recipientVoteImportedFiles: {skip: isNotRunOnUserDatabase},
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
    revokePrivilegesFromRole: {skip: isAuthCommand},
    revokeRolesFromRole: {skip: isAuthCommand},
    revokeRolesFromUser: {skip: isAuthCommand},
    rolesInfo: {skip: isNotWriteCommand},
    rotateCertificates: {skip: isAuthCommand},
    saslContinue: {skip: isAuthCommand},
    saslStart: {skip: isAuthCommand},
    sbe: {skip: isNotRunOnUserDatabase},
    serverStatus: {skip: isNotRunOnUserDatabase},
    setAllowMigrations: {skip: isNotRunOnUserDatabase},
    setCommittedSnapshot: {skip: isNotRunOnUserDatabase},
    setDefaultRWConcern: {skip: isNotRunOnUserDatabase},
    setFeatureCompatibilityVersion: {skip: isNotRunOnUserDatabase},
    setFreeMonitoring: {skip: isNotRunOnUserDatabase},
    setIndexCommitQuorum: {skip: isNotRunOnUserDatabase},
    setParameter: {skip: isNotRunOnUserDatabase},
    setShardVersion: {skip: isNotRunOnUserDatabase},
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
    update: {
        testInTransaction: true,
        testAsRetryableWrite: true,
        setUp: insertTestDoc,
        command: function(dbName, collName) {
            return {
                update: collName,
                updates: [{
                    q: TenantMigrationConcurrentWriteUtil.kTestDoc,
                    u: {$set: {y: 0}},
                    upsert: false,
                    multi: false
                }]
            };
        },
        isBatchWrite: true,
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db,
                                collName,
                                Object.assign({y: 0}, TenantMigrationConcurrentWriteUtil.kTestDoc)),
                      1);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db,
                                collName,
                                Object.assign({y: 0}, TenantMigrationConcurrentWriteUtil.kTestDoc)),
                      0);
        }
    },
    multiUpdate: {
        testInTransaction: true,
        testAsRetryableWrite: false,
        setUp: insertTwoTestDocs,
        command: function(dbName, collName) {
            return {
                update: collName,
                updates: [{q: {}, u: {$set: {y: 0}}, upsert: false, multi: true}]
            };
        },
        isBatchWrite: true,
        isMultiUpdate: true,
        assertCommandSucceeded: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, Object.assign({y: 0})), 2);
        },
        assertCommandFailed: function(db, dbName, collName) {
            assert.eq(countDocs(db, collName, Object.assign({y: 0})), 0);
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
    assert(testCase.testInTransaction ? typeof (testCase.testInTransaction) === "boolean" : true);
    assert(testCase.testAsRetryableWrite ? typeof (testCase.testAsRetryableWrite) === "boolean"
                                         : true);
}

// Validate test cases for all commands.
for (let command of Object.keys(TenantMigrationConcurrentWriteUtil.testCases)) {
    validateTestCase(TenantMigrationConcurrentWriteUtil.testCases[command]);
}
