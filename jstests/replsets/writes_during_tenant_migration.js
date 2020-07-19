/**
 * Tests that the donor rejects writes that are executed while the migration is in the blocking
 * state.
 *
 * @tags: [requires_fcv_46]
 */
(function() {
'use strict';

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
const kRecipientConnString = "testConnString";

function startMigration(primary, dbName) {
    assert.commandWorked(primary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    }));
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

function testWriteCommandSucceeded(testCase, testOpts) {
    runCommand(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

function testWriteCommandBlocked(testCase, testOpts) {
    startMigration(testOpts.primaryDB, testOpts.dbName);
    runCommand(testOpts, ErrorCodes.TenantMigrationConflict);
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
    isMaster: {skip: isNotRunOnUserDatabase},
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

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const kDbPrefix = "testDb";
const kCollName = "testColl";

// Validate test cases for all commands.
for (let command of Object.keys(testCases)) {
    validateTestCase(testCases[command]);
}

// Run test cases.
const testFuncs = {
    noTenantMigrationActive: testWriteCommandSucceeded,  // verify that the test cases are correct.
    tenantMigrationInBlocking: testWriteCommandBlocked,
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (let command of Object.keys(testCases)) {
        let testCase = testCases[command];
        let baseDbName = kDbPrefix + "-" + testName + "-" + command;

        if (testCase.skip) {
            print("Skipping " + command + ": " + testCase.skip);
            continue;
        }

        runTest(primary, testCase, testFunc, baseDbName + "Basic", kCollName);

        // TODO (SERVER-49844): Test transactional writes during migration.
        if (testCase.isSupportedInTransaction && testName == "noTenantMigrationActive") {
            runTest(
                primary, testCase, testFunc, baseDbName + "Txn", kCollName, {useTransaction: true});
        }

        if (testCase.isRetryableWriteCommand) {
            runTest(primary, testCase, testFunc, baseDbName + "Retryable", kCollName, {
                useRetryableWrite: true
            });
        }
    }
}

rst.stopSet();
})();
