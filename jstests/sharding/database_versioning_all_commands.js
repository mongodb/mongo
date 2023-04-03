/**
 * Specifies for each command whether it is expected to send a databaseVersion, and verifies that
 * the commands match the specification.
 */
(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");
load('jstests/sharding/libs/last_lts_mongos_commands.js');

function getNewDbName(dbName) {
    if (!getNewDbName.counter) {
        getNewDbName.counter = 0;
    }
    getNewDbName.counter++;
    return "db" + getNewDbName.counter;
}

function assertMongosDatabaseVersion(conn, dbName, dbVersion) {
    let res = conn.adminCommand({getShardVersion: dbName});
    assert.commandWorked(res);
    assert.eq(dbVersion, res.version);
}

function assertShardDatabaseVersion(shard, dbName, dbVersion) {
    let res = shard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.eq(dbVersion, res.dbVersion);
}

function containsCollection(shard, dbName, collName) {
    let res = shard.getDB(dbName).runCommand({listCollections: 1});
    assert.commandWorked(res);
    let collections = res.cursor.firstBatch;
    for (let collection of collections) {
        if (collection["name"] === collName) {
            return true;
        }
    }
    return false;
}

function validateTestCase(testCase) {
    assert(testCase.skip || testCase.run,
           "must specify exactly one of 'skip' or 'run' for test case " + tojson(testCase));

    if (testCase.skip) {
        for (let key of Object.keys(testCase)) {
            assert(
                key === "skip" || key === "conditional",
                "if a test case specifies 'skip', it must not specify any other fields besides 'conditional': " +
                    key + ": " + tojson(testCase));
        }
        return;
    }

    validateCommandTestCase(testCase.run);

    if (testCase.explain) {
        validateCommandTestCase(testCase.explain);
    }
}

function validateCommandTestCase(testCase) {
    assert(testCase.command, "must specify 'command' for test case " + tojson(testCase));

    // Check that required fields are present.
    assert(testCase.hasOwnProperty("sendsDbVersion"),
           "must specify 'sendsDbVersion' for test case " + tojson(testCase));

    // Check that all present fields are of the correct type.
    assert(typeof (testCase.command) === "function");
    assert(testCase.runsAgainstAdminDb ? typeof (testCase.runsAgainstAdminDb) === "boolean" : true);
    assert(typeof (testCase.sendsDbVersion) === "boolean");
    assert(testCase.explicitlyCreateCollection
               ? typeof (testCase.explicitlyCreateCollection) === "boolean"
               : true);
    assert(testCase.expectNonEmptyCollection
               ? typeof (testCase.expectNonEmptyCollection) === "boolean"
               : true);
    assert(testCase.cleanUp ? typeof (testCase.cleanUp) === "function" : true,
           "cleanUp must be a function: " + tojson(testCase));
}

function testCommandAfterMovePrimary(testCase, st, dbName, collName) {
    const command = testCase.command(dbName, collName);

    const primaryShardBefore = st.getPrimaryShard(dbName);
    const primaryShardAfter = st.getOther(primaryShardBefore);
    const dbVersionBefore =
        st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    jsTest.log("testing command " + tojson(command) + " after movePrimary; primary shard before: " +
               primaryShardBefore + ", database version before: " + tojson(dbVersionBefore) +
               ", primary shard after: " + primaryShardAfter);

    if (testCase.explicitlyCreateCollection) {
        assert.commandWorked(primaryShardBefore.getDB(dbName).runCommand({create: collName}));
    }
    if (testCase.expectNonEmptyCollection) {
        assert.commandWorked(
            primaryShardBefore.getDB(dbName).runCommand({insert: collName, documents: [{x: 0}]}));
    }

    // Ensure all nodes know the dbVersion before the movePrimary.
    assert.commandWorked(st.s0.adminCommand({flushRouterConfig: 1}));
    assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assert.commandWorked(primaryShardBefore.adminCommand({_flushDatabaseCacheUpdates: dbName}));
    assertShardDatabaseVersion(primaryShardBefore, dbName, dbVersionBefore);
    assert.commandWorked(primaryShardAfter.adminCommand({_flushDatabaseCacheUpdates: dbName}));
    assertShardDatabaseVersion(primaryShardAfter, dbName, dbVersionBefore);

    // Run movePrimary through the second mongos.
    assert.commandWorked(st.s1.adminCommand({movePrimary: dbName, to: primaryShardAfter.name}));
    const dbVersionAfter =
        st.s1.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    // After the movePrimary, both old and new primary shards should have cleared the dbVersion.
    assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertShardDatabaseVersion(primaryShardBefore, dbName, {});
    // TODO (SERVER-71309): Remove once 7.0 becomes last LTS.
    if (FeatureFlagUtil.isEnabled(st.configRS.getPrimary().getDB('admin'),
                                  "ResilientMovePrimary")) {
        assertShardDatabaseVersion(primaryShardAfter, dbName, {});
    } else {
        assertShardDatabaseVersion(primaryShardAfter, dbName, dbVersionBefore);
    }

    // Run the test case's command.
    const res = st.s0.getDB(testCase.runsAgainstAdminDb ? "admin" : dbName).runCommand(command);
    if (testCase.expectedFailureCode) {
        assert.commandFailedWithCode(res, testCase.expectedFailureCode);
    } else {
        assert.commandWorked(res);
    }

    if (testCase.sendsDbVersion) {
        // If the command participates in database versioning, all nodes should now know the new
        // dbVersion:
        // 1. The mongos should have sent the stale dbVersion to the old primary shard
        // 2. The old primary shard should have returned StaleDbVersion and refreshed
        // 3. Which should have caused the mongos to refresh and retry against the new primary shard
        // 4. The new primary shard should have returned StaleDbVersion and refreshed
        // 5. Which should have caused the mongos to refresh and retry again, this time succeeding.
        assertMongosDatabaseVersion(st.s0, dbName, dbVersionAfter);
        assertShardDatabaseVersion(primaryShardBefore, dbName, dbVersionAfter);
        assertShardDatabaseVersion(primaryShardAfter, dbName, dbVersionAfter);
    } else {
        // If the command does not participate in database versioning:
        // 1. The mongos should have targeted the old primary shard but not attached a dbVersion
        // 2. The old primary shard should have returned an ok response
        // 3. Both old and new primary shards should have cleared the dbVersion
        assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
        assertShardDatabaseVersion(primaryShardBefore, dbName, {});
        // TODO (SERVER-71309): Remove once 7.0 becomes last LTS.
        if (FeatureFlagUtil.isEnabled(st.configRS.getPrimary().getDB('admin'),
                                      "ResilientMovePrimary")) {
            assertShardDatabaseVersion(primaryShardAfter, dbName, {});
        } else {
            assertShardDatabaseVersion(primaryShardAfter, dbName, dbVersionBefore);
        }
    }

    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    } else {
        assert(st.s0.getDB(dbName).getCollection(collName).drop());
    }
}

function testCommandAfterDropRecreateDatabase(testCase, st) {
    const dbName = getNewDbName();
    const collName = "foo";
    const command = testCase.command(dbName, collName);

    // Create the database by creating a collection in it.
    assert.commandWorked(st.s0.getDB(dbName).createCollection(collName));
    let dbVersionBefore =
        st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;
    let primaryShardBefore = st.getPrimaryShard(dbName);
    let primaryShardAfter = st.getOther(primaryShardBefore);

    jsTest.log("testing command " + tojson(command) +
               " after drop/recreate database; primary shard before: " + primaryShardBefore +
               ", database version before: " + tojson(dbVersionBefore) +
               ", primary shard after: " + primaryShardAfter);

    // Ensure the router and primary shard know the dbVersion before the drop/recreate database.
    assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertShardDatabaseVersion(primaryShardBefore, dbName, dbVersionBefore);
    assertShardDatabaseVersion(primaryShardAfter, dbName, {});

    // Drop and recreate the database through the second mongos. Insert the entry for the new
    // database explicitly to ensure it is assigned the other shard as the primary shard.
    assert.commandWorked(st.s1.getDB(dbName).dropDatabase());
    let currDbVersion = {
        uuid: UUID(),
        timestamp: Timestamp(dbVersionBefore.timestamp.getTime() + 1, 0),
        lastMod: NumberInt(1)
    };
    assert.commandWorked(st.s1.getDB("config").getCollection("databases").insert({
        _id: dbName,
        partitioned: false,
        primary: primaryShardAfter.shardName,
        version: currDbVersion
    }));

    const dbVersionAfter =
        st.s1.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    if (testCase.explicitlyCreateCollection) {
        assert.commandWorked(primaryShardAfter.getDB(dbName).runCommand({create: collName}));
    }
    if (testCase.expectNonEmptyCollection) {
        assert.commandWorked(
            primaryShardAfter.getDB(dbName).runCommand({insert: collName, documents: [{x: 0}]}));
    }

    // The only change after the drop/recreate database should be that the old primary shard should
    // have cleared its dbVersion.
    assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertShardDatabaseVersion(primaryShardBefore, dbName, {});
    assertShardDatabaseVersion(primaryShardAfter, dbName, {});

    // Run the test case's command.
    const res = st.s0.getDB(testCase.runsAgainstAdminDb ? "admin" : dbName).runCommand(command);
    if (testCase.expectedFailureCode) {
        assert.commandFailedWithCode(res, testCase.expectedFailureCode);
    } else {
        assert.commandWorked(res);
    }

    if (testCase.sendsDbVersion) {
        // If the command participates in database versioning all nodes should now know the new
        // dbVersion:
        // 1. The mongos should have sent the stale dbVersion to the old primary shard
        // 2. The old primary shard should have returned StaleDbVersion and refreshed
        // 3. Which should have caused the mongos to refresh and retry against the new primary shard
        // 4. The new primary shard should have returned StaleDbVersion and refreshed
        // 5. Which should have caused the mongos to refresh and retry again, this time succeeding.
        assertMongosDatabaseVersion(st.s0, dbName, dbVersionAfter);
        assertShardDatabaseVersion(primaryShardBefore, dbName, dbVersionAfter);
        assertShardDatabaseVersion(primaryShardAfter, dbName, dbVersionAfter);
    } else {
        // If the command does not participate in database versioning, none of the nodes' view of
        // the dbVersion should have changed:
        // 1. The mongos should have targeted the old primary shard but not attached a dbVersion
        // 2. The old primary shard should have returned an ok response
        assertMongosDatabaseVersion(st.s0, dbName, dbVersionBefore);
        assertShardDatabaseVersion(primaryShardBefore, dbName, {});
        assertShardDatabaseVersion(primaryShardAfter, dbName, {});
    }

    // Clean up.
    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    } else {
        assert(st.s0.getDB(dbName).getCollection(collName).drop());
    }
    assert.commandWorked(st.s0.getDB(dbName).dropDatabase());
}

let testCases = {
    _clusterQueryWithoutShardKey:
        {skip: "executed locally on a mongos (not sent to any remote node)"},
    _clusterWriteWithoutShardKey:
        {skip: "executed locally on a mongos (not sent to any remote node)"},
    _getAuditConfigGeneration: {skip: "not on a user database", conditional: true},
    _hashBSONElement: {skip: "executes locally on mongos (not sent to any remote node)"},
    _isSelf: {skip: "executes locally on mongos (not sent to any remote node)"},
    _killOperations: {skip: "executes locally on mongos (not sent to any remote node)"},
    _mergeAuthzCollections: {skip: "always targets the config server"},
    abortReshardCollection: {skip: "always targets the config server"},
    abortTransaction: {skip: "unversioned and uses special targetting rules"},
    addShard: {skip: "not on a user database"},
    addShardToZone: {skip: "not on a user database"},
    aggregate: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}};
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    explain:
                        {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}}
                };
            }
        }
    },
    analyze: {
        skip: "unimplemented. Serves only as a stub."
    },  // TODO SERVER-68055: Extend test to work with analyze
    analyzeShardKey: {
        run: {
            runsAgainstAdminDb: true,
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            expectNonEmptyCollection: true,
            // The command should fail while calculating the read and write distribution metrics
            // since the cardinality of the shard key is less than analyzeShardKeyNumRanges which
            // defaults to 100.
            expectedFailureCode: 4952606,
            command: function(dbName, collName) {
                return {analyzeShardKey: dbName + "." + collName, key: {_id: 1}};
            },
        }
    },
    appendOplogNote: {skip: "unversioned and executes on all shards"},
    authenticate: {skip: "does not forward command to primary shard"},
    balancerCollectionStatus: {skip: "does not forward command to primary shard"},
    balancerStart: {skip: "not on a user database"},
    balancerStatus: {skip: "not on a user database"},
    balancerStop: {skip: "not on a user database"},
    buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    bulkWrite: {skip: "not yet implemented"},
    checkMetadataConsistency: {skip: "not yet implemented"},
    cleanupReshardCollection: {skip: "always targets the config server"},
    clearJumboFlag: {skip: "does not forward command to primary shard"},
    clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    collMod: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {collMod: collName};
            },
        }
    },
    collStats: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {collStats: collName};
            },
        }
    },
    commitReshardCollection: {skip: "always targets the config server"},
    commitTransaction: {skip: "unversioned and uses special targetting rules"},
    compact: {skip: "not allowed through mongos"},
    compactStructuredEncryptionData: {skip: "requires encrypted collections"},
    configureCollectionBalancing: {skip: "always targets the config server"},
    configureFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
    configureQueryAnalyzer: {skip: "always targets the config server"},
    connPoolStats: {skip: "executes locally on mongos (not sent to any remote node)"},
    connPoolSync: {skip: "executes locally on mongos (not sent to any remote node)"},
    connectionStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    convertToCapped: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {convertToCapped: collName, size: 8192};
            },
        }
    },
    coordinateCommitTransaction: {skip: "unimplemented. Serves only as a stub."},
    count: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {count: collName, query: {x: 1}};
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {explain: {count: collName, query: {x: 1}}};
            }
        }
    },
    create: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {create: collName};
            },
        }
    },
    createIndexes: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]};
            },
        }
    },
    createSearchIndexes: {skip: "executes locally on mongos"},
    createRole: {skip: "always targets the config server"},
    createUser: {skip: "always targets the config server"},
    currentOp: {skip: "not on a user database"},
    dataSize: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {dataSize: dbName + "." + collName};
            },
        }
    },
    dbStats: {
        run: {
            // dbStats is always broadcast to all shards
            sendsDbVersion: false,
            command: function(dbName, collName) {
                return {dbStats: 1, scale: 1};
            }
        }
    },
    delete: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {explain: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}};
            }
        }
    },
    distinct: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {distinct: collName, key: "x"};
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {explain: {distinct: collName, key: "x"}};
            }
        },
    },
    drop: {skip: "does not forward command to primary shard"},
    dropAllRolesFromDatabase: {skip: "always targets the config server"},
    dropAllUsersFromDatabase: {skip: "always targets the config server"},
    dropConnections: {skip: "not on a user database"},
    dropDatabase: {skip: "drops the database from the cluster, changing the UUID"},
    dropIndexes: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {dropIndexes: collName, index: "*"};
            },
        }
    },
    dropRole: {skip: "always targets the config server"},
    dropSearchIndex: {skip: "executes locally on mongos"},
    dropUser: {skip: "always targets the config server"},
    echo: {skip: "does not forward command to primary shard"},
    enableSharding: {skip: "does not forward command to primary shard"},
    endSessions: {skip: "goes through the cluster write path"},
    explain: {skip: "TODO SERVER-31226"},
    features: {skip: "executes locally on mongos (not sent to any remote node)"},
    filemd5: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {filemd5: ObjectId(), root: collName};
            }
        }
    },
    find: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {find: collName, filter: {x: 1}};
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {explain: {find: collName, filter: {x: 1}}};
            }
        }
    },
    findAndModify: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {findAndModify: collName, query: {_id: 0}, remove: true};
            }
        },
        explain: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {explain: {findAndModify: collName, query: {_id: 0}, remove: true}};
            }
        }
    },
    flushRouterConfig: {skip: "executes locally on mongos (not sent to any remote node)"},
    fsync: {skip: "broadcast to all shards"},
    getAuditConfig: {skip: "not on a user database", conditional: true},
    getClusterParameter: {skip: "always targets the config server"},
    getCmdLineOpts: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDefaultRWConcern: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDiagnosticData: {skip: "executes locally on mongos (not sent to any remote node)"},
    getLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    getMore: {skip: "requires a previously established cursor"},
    getParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    getQueryableEncryptionCountInfo: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    getQueryableEncryptionCountInfo: collName,
                    tokens: [
                        {
                            tokens:
                                [{"s": BinData(0, "lUBO7Mov5Sb+c/D4cJ9whhhw/+PZFLCk/AQU2+BpumQ=")}]
                        },
                    ],
                    "forInsert": true
                };
            }
        }
    },
    getShardMap: {skip: "executes locally on mongos (not sent to any remote node)"},
    getShardVersion: {skip: "executes locally on mongos (not sent to any remote node)"},
    grantPrivilegesToRole: {skip: "always targets the config server"},
    grantRolesToRole: {skip: "always targets the config server"},
    grantRolesToUser: {skip: "always targets the config server"},
    hello: {skip: "executes locally on mongos (not sent to any remote node)"},
    hostInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    insert: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {insert: collName, documents: [{_id: 1}]};
            },
        }
    },
    invalidateUserCache: {skip: "executes locally on mongos (not sent to any remote node)"},
    isdbgrid: {skip: "executes locally on mongos (not sent to any remote node)"},
    isMaster: {skip: "executes locally on mongos (not sent to any remote node)"},
    killCursors: {skip: "requires a previously established cursor"},
    killAllSessions: {skip: "always broadcast to all hosts in the cluster"},
    killAllSessionsByPattern: {skip: "always broadcast to all hosts in the cluster"},
    killOp: {skip: "does not forward command to primary shard"},
    killSessions: {skip: "always broadcast to all hosts in the cluster"},
    listCollections: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {listCollections: 1};
            }
        }
    },
    listCommands: {skip: "executes locally on mongos (not sent to any remote node)"},
    listDatabases: {skip: "does not forward command to primary shard"},
    listIndexes: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {listIndexes: collName};
            },
        }
    },
    listSearchIndexes: {skip: "executes locally on mongos"},
    listShards: {skip: "does not forward command to primary shard"},
    logApplicationMessage: {skip: "not on a user database", conditional: true},
    logMessage: {skip: "not on a user database"},
    logRotate: {skip: "executes locally on mongos (not sent to any remote node)"},
    logout: {skip: "not on a user database"},
    mapReduce: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    mapReduce: collName,
                    map: function mapFunc() {
                        emit(this.x, 1);
                    },
                    reduce: function reduceFunc(key, values) {
                        return Array.sum(values);
                    },
                    out: "inline"
                };
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    explain: {
                        mapReduce: collName,
                        map: function mapFunc() {
                            emit(this.x, 1);
                        },
                        reduce: function reduceFunc(key, values) {
                            return Array.sum(values);
                        },
                        out: "inline"
                    }
                };
            }
        }
    },
    mergeAllChunksOnShard: {skip: "does not forward command to primary shard"},
    mergeChunks: {skip: "does not forward command to primary shard"},
    moveChunk: {skip: "does not forward command to primary shard"},
    movePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
    moveRange: {skip: "does not forward command to primary shard"},
    multicast: {skip: "does not forward command to primary shard"},
    netstat: {skip: "executes locally on mongos (not sent to any remote node)"},
    oidcListKeys: {skip: "executes locally on mongos (not sent to any remote node)"},
    oidcRefreshKeys: {skip: "executes locally on mongos (not sent to any remote node)"},
    ping: {skip: "executes locally on mongos (not sent to any remote node)"},
    planCacheClear: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {planCacheClear: collName};
            }
        }
    },
    planCacheClearFilters: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {planCacheClearFilters: collName};
            }
        }
    },
    planCacheListFilters: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {planCacheListFilters: collName};
            }
        }
    },
    planCacheSetFilter: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]};
            },
        }
    },
    profile: {skip: "not supported in mongos"},
    reapLogicalSessionCacheNow: {skip: "is a no-op on mongos"},
    refineCollectionShardKey: {skip: "not on a user database"},
    refreshLogicalSessionCacheNow: {skip: "goes through the cluster write path"},
    refreshSessions: {skip: "executes locally on mongos (not sent to any remote node)"},
    refreshSessionsInternal:
        {skip: "executes locally on mongos (not sent to any remote node)", conditional: true},
    removeShard: {skip: "not on a user database"},
    removeShardFromZone: {skip: "not on a user database"},
    renameCollection: {
        run: {
            runsAgainstAdminDb: true,
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {
                    renameCollection: dbName + "." + collName,
                    to: dbName + "." + collName + "_renamed"
                };
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName + "_renamed").drop());
            }
        }
    },
    repairShardedCollectionChunksHistory: {skip: "always targets the config server"},
    replSetGetStatus: {skip: "not supported in mongos"},
    reshardCollection: {skip: "does not forward command to primary shard"},
    revokePrivilegesFromRole: {skip: "always targets the config server"},
    revokeRolesFromRole: {skip: "always targets the config server"},
    revokeRolesFromUser: {skip: "always targets the config server"},
    rolesInfo: {skip: "always targets the config server"},
    rotateCertificates: {skip: "executes locally on mongos (not sent to any remote node)"},
    saslContinue: {skip: "not on a user database"},
    saslStart: {skip: "not on a user database"},
    serverStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    setAllowMigrations: {skip: "not on a user database"},
    setAuditConfig: {skip: "not on a user database", conditional: true},
    setDefaultRWConcern: {skip: "always targets the config server"},
    setIndexCommitQuorum: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            // The command should fail if there is no active index build on the collection.
            expectedFailureCode: ErrorCodes.IndexNotFound,
            command: function(dbName, collName) {
                return {
                    setIndexCommitQuorum: collName,
                    indexNames: ["index"],
                    commitQuorum: "majority"
                };
            },
        }
    },
    setFeatureCompatibilityVersion: {skip: "not on a user database"},
    setFreeMonitoring:
        {skip: "explicitly fails for mongos, primary mongod only", conditional: true},
    setProfilingFilterGlobally: {skip: "executes locally on mongos (not sent to any remote node)"},
    setParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    setClusterParameter: {skip: "always targets the config server"},
    setUserWriteBlockMode: {skip: "executes locally on mongos (not sent to any remote node)"},
    shardCollection: {skip: "does not forward command to primary shard"},
    shutdown: {skip: "does not forward command to primary shard"},
    split: {skip: "does not forward command to primary shard"},
    splitVector: {skip: "does not forward command to primary shard"},
    startRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
    startSession: {skip: "executes locally on mongos (not sent to any remote node)"},
    stopRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
    testDeprecation: {skip: "executes locally on mongos (not sent to any remote node)"},
    testDeprecationInVersion2: {skip: "executes locally on mongos (not sent to any remote node)"},
    testInternalTransactions: {skip: "executes locally on mongos (not sent to any remote node)"},
    testRemoval: {skip: "executes locally on mongos (not sent to any remote node)"},
    testVersion2: {skip: "executes locally on mongos (not sent to any remote node)"},
    testVersions1And2: {skip: "executes locally on mongos (not sent to any remote node)"},
    transitionToCatalogShard: {skip: "not on a user database"},
    transitionToDedicatedConfigServer: {skip: "not on a user database"},
    update: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    update: collName,
                    updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
                };
            }
        },
        explain: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {
                    explain: {
                        update: collName,
                        updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
                    }
                };
            },
        }
    },
    updateRole: {skip: "always targets the config server"},
    updateSearchIndex: {skip: "executes locally on mongos"},
    updateUser: {skip: "always targets the config server"},
    updateZoneKeyRange: {skip: "not on a user database"},
    usersInfo: {skip: "always targets the config server"},
    validate: {
        run: {
            sendsDbVersion: true,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {validate: collName};
            },
        }
    },
    validateDBMetadata: {
        run: {
            // validateDBMetadata is always broadcast to all shards.
            sendsDbVersion: false,
            explicitlyCreateCollection: true,
            command: function(dbName, collName) {
                return {validateDBMetadata: 1, apiParameters: {version: "1"}};
            },
        }
    },
    waitForFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
    whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
};

commandsRemovedFromMongosSinceLastLTS.forEach(function(cmd) {
    testCases[cmd] = {skip: "must define test coverage for latest version backwards compatibility"};
});

const st = new ShardingTest({shards: 2, mongos: 2});

const listCommandsRes = st.s0.adminCommand({listCommands: 1});
assert.commandWorked(listCommandsRes);
print("--------------------------------------------");
for (let command of Object.keys(listCommandsRes.commands)) {
    print(command);
}

(() => {
    // Validate test cases for all commands.

    // Ensure there is a test case for every mongos command, and that the test cases are well
    // formed.
    for (let command of Object.keys(listCommandsRes.commands)) {
        let testCase = testCases[command];
        assert(testCase !== undefined, "coverage failure: must define a test case for " + command);
        validateTestCase(testCase);
        testCases[command].validated = true;
    }

    // After iterating through all the existing commands, ensure there were no additional test cases
    // that did not correspond to any mongos command.
    for (let key of Object.keys(testCases)) {
        // We have defined real test cases for commands added since the last LTS version so that the
        // test cases are exercised in the regular suites, but because these test cases can't run in
        // the last stable suite, we skip processing them here to avoid failing the below assertion.
        // We have defined "skip" test cases for commands removed since the last LTS version so the
        // test case is defined in last stable suites (in which these commands still exist on the
        // mongos), but these test cases won't be run in regular suites, so we skip processing them
        // below as well.
        if (commandsAddedToMongosSinceLastLTS.includes(key) ||
            commandsRemovedFromMongosSinceLastLTS.includes(key)) {
            continue;
        }
        assert(testCases[key].validated || testCases[key].conditional,
               "you defined a test case for a command '" + key +
                   "' that does not exist on mongos: " + tojson(testCases[key]));
    }
})();

(() => {
    // Test that commands that send databaseVersion are subjected to the databaseVersion check when
    // the primary shard for the database has moved and the database no longer exists on the old
    // primary shard (because the database only contained unsharded collections; this is in
    // anticipation of SERVER-43925).

    const dbName = getNewDbName();
    const collName = "foo";

    // Create the database by creating a collection in it.
    assert.commandWorked(st.s0.getDB(dbName).createCollection("dummy"));

    for (let command of Object.keys(listCommandsRes.commands)) {
        let testCase = testCases[command];
        if (testCase.skip) {
            print("skipping " + command + ": " + testCase.skip);
            continue;
        }

        testCommandAfterMovePrimary(testCase.run, st, dbName, collName);
        if (testCase.explain) {
            testCommandAfterMovePrimary(testCase.explain, st, dbName, collName);
        }
    }
})();

(() => {
    // Test that commands that send databaseVersion are subjected to the databaseVersion check when
    // the primary shard for the database has moved, but the database still exists on the old
    // primary shard (because the old primary shard owns chunks for sharded collections in the
    // database).

    const dbName = getNewDbName();
    const collName = "foo";
    const shardedCollName = "pinnedShardedCollWithChunksOnBothShards";

    // Create a sharded collection with data on both shards so that the database does not get
    // dropped on the old primary shard after movePrimary.
    let shardedCollNs = dbName + "." + shardedCollName;
    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard0.shardName, zone: 'x < 0'}));
    assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard1.shardName, zone: 'x >= 0'}));
    assert.commandWorked(st.s0.adminCommand(
        {updateZoneKeyRange: shardedCollNs, min: {x: MinKey}, max: {x: 0}, zone: 'x < 0'}));
    assert.commandWorked(st.s0.adminCommand(
        {updateZoneKeyRange: shardedCollNs, min: {x: 0}, max: {x: MaxKey}, zone: 'x >= 0'}));
    assert.commandWorked(
        st.s0.getDB('admin').admin.runCommand({shardCollection: shardedCollNs, key: {"x": 1}}));
    assert(containsCollection(st.shard0, dbName, shardedCollName));
    assert(containsCollection(st.shard1, dbName, shardedCollName));

    for (let command of Object.keys(listCommandsRes.commands)) {
        let testCase = testCases[command];
        if (testCase.skip) {
            print("skipping " + command + ": " + testCase.skip);
            continue;
        }

        testCommandAfterMovePrimary(testCase.run, st, dbName, collName);
        if (testCase.explain) {
            testCommandAfterMovePrimary(testCase.explain, st, dbName, collName);
        }
    }
})();

(() => {
    // Test that commands that send databaseVersion are subjected to the databaseVersion check when
    // the database has been dropped and recreated with a different primary shard.

    for (let command of Object.keys(listCommandsRes.commands)) {
        let testCase = testCases[command];
        if (testCase.skip) {
            print("skipping " + command + ": " + testCase.skip);
            continue;
        }

        testCommandAfterDropRecreateDatabase(testCase.run, st);
        if (testCase.explain) {
            testCommandAfterDropRecreateDatabase(testCase.explain, st);
        }
    }
})();

st.stop();
})();
