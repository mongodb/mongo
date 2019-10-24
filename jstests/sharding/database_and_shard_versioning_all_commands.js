/**
 * Specifies for each command whether it is expected to send a databaseVersion, and verifies that
 * the commands match the specification.
 */
(function() {
'use strict';

load('jstests/sharding/libs/last_stable_mongos_commands.js');

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

function containsDatabase(shard, dbName) {
    let res = shard.adminCommand({listDatabases: 1});
    assert.commandWorked(res);
    for (let database of res.databases) {
        if (database["name"] == dbName) {
            return true;
        }
    }
    return false;
}

function containsCollection(shard, dbName, collName) {
    let res = shard.getDB(dbName).runCommand({listCollections: 1});
    assert.commandWorked(res);
    let collections = res.cursor.firstBatch;
    for (let collection of collections) {
        if (collection["name"] == collName) {
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
    assert(testCase.setUp ? typeof (testCase.setUp) === "function" : true,
           "setUp must be a function: " + tojson(testCase));
    assert(testCase.cleanUp ? typeof (testCase.cleanUp) === "function" : true,
           "cleanUp must be a function: " + tojson(testCase));
}

function runCommandTestCaseSendDbVersion(testCase, st, routingInfo, dbName, collName) {
    let command = testCase.command(dbName, collName);
    jsTest.log("testing command " + tojson(command));

    if (testCase.setUp) {
        testCase.setUp(st.s0, dbName, collName);
    }

    if (testCase.runsAgainstAdminDb) {
        assert.commandWorked(st.s0.adminCommand(command));
    } else {
        assert.commandWorked(st.s0.getDB(dbName).runCommand(command));
    }

    if (testCase.sendsDbVersion) {
        assertShardDatabaseVersion(routingInfo.primaryShard, dbName, routingInfo.dbVersion);
    } else {
        assertShardDatabaseVersion(routingInfo.primaryShard, dbName, {});
    }

    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    }

    // Ensure the primary shard's database entry is stale for the next command by changing the
    // primary shard (the recipient shard does not refresh until getting a request with the new
    // version).
    let fromShard = st.getPrimaryShard(dbName);
    let toShard = st.getOther(fromShard);

    routingInfo.primaryShard = toShard;
    const prevDbVersion = routingInfo.dbVersion;

    assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: toShard.name}));
    routingInfo.dbVersion =
        st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    // The dbVersion should have changed due to the movePrimary operation.
    assert.eq(routingInfo.dbVersion.lastMod, prevDbVersion.lastMod + 1);

    // The fromShard should have cleared its in-memory database info but still have the database.
    assertShardDatabaseVersion(fromShard, dbName, {});
    assert(containsDatabase(fromShard, dbName));
}

function runCommandTestCaseCheckDbVersion(testCase, st, dbName, collName) {
    let command = testCase.command(dbName, collName);
    if (!testCase.sendsDbVersion) {
        return;
    }
    jsTest.log("testing command " + tojson(command));

    // Create and drop the database to create a stale entry for the database in the first mongos's
    // cache, and manually insert an entry for the database in config.databases for a different
    // shard.
    assert.commandWorked(st.s0.getDB(dbName).createCollection(collName));
    let prevDbVersion =
        st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;
    let prevPrimaryShard = st.getPrimaryShard(dbName);
    let currPrimaryShard = st.getOther(prevPrimaryShard);

    assertMongosDatabaseVersion(st.s0, dbName, prevDbVersion);
    assertShardDatabaseVersion(prevPrimaryShard, dbName, prevDbVersion);
    assertShardDatabaseVersion(currPrimaryShard, dbName, {});

    assert.commandWorked(st.s1.getDB(dbName).dropDatabase());
    assertMongosDatabaseVersion(st.s0, dbName, prevDbVersion);
    assertShardDatabaseVersion(prevPrimaryShard, dbName, {});
    assertShardDatabaseVersion(currPrimaryShard, dbName, {});

    let currDbVersion = {uuid: UUID(), lastMod: NumberInt(1)};
    assert.commandWorked(st.s0.getDB("config").getCollection("databases").insert({
        _id: dbName,
        partitioned: false,
        primary: currPrimaryShard.shardName,
        version: currDbVersion
    }));

    // Set up and check that the shards still do not have the database version.
    if (testCase.setUp) {
        testCase.setUp(st.s0, dbName, collName);
    }
    assertMongosDatabaseVersion(st.s0, dbName, prevDbVersion);
    assertShardDatabaseVersion(prevPrimaryShard, dbName, {});
    assertShardDatabaseVersion(currPrimaryShard, dbName, {});

    // Run the command and check that the shards have the correct database version
    // after the command returns.
    if (testCase.runsAgainstAdminDb) {
        assert.commandWorked(st.s0.adminCommand(command));
    } else {
        assert.commandWorked(st.s0.getDB(dbName).runCommand(command));
    }
    assertMongosDatabaseVersion(st.s0, dbName, currDbVersion);
    assertShardDatabaseVersion(prevPrimaryShard, dbName, currDbVersion);
    assertShardDatabaseVersion(currPrimaryShard, dbName, currDbVersion);

    // Clean up.
    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    }
    assert.commandWorked(st.s0.getDB(dbName).dropDatabase());
}

let testCases = {
    _hashBSONElement: {skip: "executes locally on mongos (not sent to any remote node)"},
    _isSelf: {skip: "executes locally on mongos (not sent to any remote node)"},
    _mergeAuthzCollections: {skip: "always targets the config server"},
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
    authenticate: {skip: "does not forward command to primary shard"},
    availableQueryOptions: {skip: "executes locally on mongos (not sent to any remote node)"},
    balancerStart: {skip: "not on a user database"},
    balancerStatus: {skip: "not on a user database"},
    balancerStop: {skip: "not on a user database"},
    buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    clearJumboFlag: {skip: "does not forward command to primary shard"},
    clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    collMod: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {collMod: collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    collStats: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {collStats: collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    commitTransaction: {skip: "unversioned and uses special targetting rules"},
    compact: {skip: "not allowed through mongos"},
    configureFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
    connPoolStats: {skip: "executes locally on mongos (not sent to any remote node)"},
    connPoolSync: {skip: "executes locally on mongos (not sent to any remote node)"},
    connectionStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    convertToCapped: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {convertToCapped: collName, size: 8192};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
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
            sendsDbVersion: false,
            command: function(dbName, collName) {
                return {create: collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    createIndexes: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    createRole: {skip: "always targets the config server"},
    createUser: {skip: "always targets the config server"},
    currentOp: {skip: "not on a user database"},
    dataSize: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {dataSize: dbName + "." + collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
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
    drop: {
        run: {
            sendsDbVersion: false,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {drop: collName};
            }
        }
    },
    dropAllRolesFromDatabase: {skip: "always targets the config server"},
    dropAllUsersFromDatabase: {skip: "always targets the config server"},
    dropConnections: {skip: "not on a user database"},
    dropDatabase: {skip: "drops the database from the cluster, changing the UUID"},
    dropIndexes: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {dropIndexes: collName, index: "*"};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    dropRole: {skip: "always targets the config server"},
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
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {explain: {findAndModify: collName, query: {_id: 0}, remove: true}};
            }
        }
    },
    flushRouterConfig: {skip: "executes locally on mongos (not sent to any remote node)"},
    fsync: {skip: "broadcast to all shards"},
    getCmdLineOpts: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDefaultRWConcern: {skip: "executes locally on mongos (not sent to any remote node)"},
    getDiagnosticData: {skip: "executes locally on mongos (not sent to any remote node)"},
    getLastError: {skip: "does not forward command to primary shard"},
    getLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    getMore: {skip: "requires a previously established cursor"},
    getParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    getShardMap: {skip: "executes locally on mongos (not sent to any remote node)"},
    getShardVersion: {skip: "executes locally on mongos (not sent to any remote node)"},
    getnonce: {skip: "not on a user database"},
    grantPrivilegesToRole: {skip: "always targets the config server"},
    grantRolesToRole: {skip: "always targets the config server"},
    grantRolesToUser: {skip: "always targets the config server"},
    hostInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    insert: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {insert: collName, documents: [{_id: 1}]};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                // Implicitly creates the collection.
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
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
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {listIndexes: collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    listShards: {skip: "does not forward command to primary shard"},
    logApplicationMessage: {skip: "not on a user database", conditional: true},
    logRotate: {skip: "executes locally on mongos (not sent to any remote node)"},
    logout: {skip: "not on a user database"},
    mapReduce: {
        run: {
            // mapReduce uses connection versioning, which does not support database versioning.
            sendsDbVersion: false,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {
                    mapReduce: collName,
                    map: function() {
                        emit(this.x, 1);
                    },
                    reduce: function(key, values) {
                        return Array.sum(values);
                    },
                    out: {inline: 1}
                };
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
                assert(mongosConn.getDB(dbName).getCollection(collName + "_renamed").drop());
            }
        }
    },
    mergeChunks: {skip: "does not forward command to primary shard"},
    moveChunk: {skip: "does not forward command to primary shard"},
    movePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
    multicast: {skip: "does not forward command to primary shard"},
    netstat: {skip: "executes locally on mongos (not sent to any remote node)"},
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
    planCacheListPlans: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {planCacheListPlans: collName, query: {_id: "A"}};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    planCacheListQueryShapes: {
        run: {
            sendsDbVersion: true,
            command: function(dbName, collName) {
                return {planCacheListQueryShapes: collName};
            }
        }
    },
    planCacheSetFilter: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
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
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
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
    replSetGetStatus: {skip: "not supported in mongos"},
    resetError: {skip: "not on a user database"},
    restartCatalog: {skip: "not on a user database"},
    revokePrivilegesFromRole: {skip: "always targets the config server"},
    revokeRolesFromRole: {skip: "always targets the config server"},
    revokeRolesFromUser: {skip: "always targets the config server"},
    rolesInfo: {skip: "always targets the config server"},
    saslContinue: {skip: "not on a user database"},
    saslStart: {skip: "not on a user database"},
    serverStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
    setDefaultRWConcern: {skip: "always targets the config server"},
    setIndexCommitQuorum: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {
                    setIndexCommitQuorum: collName,
                    indexNames: ["index"],
                    commitQuorum: "majority"
                };
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    setFeatureCompatibilityVersion: {skip: "not on a user database"},
    setFreeMonitoring:
        {skip: "explicitly fails for mongos, primary mongod only", conditional: true},
    setParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
    shardCollection: {skip: "does not forward command to primary shard"},
    shardConnPoolStats: {skip: "does not forward command to primary shard"},
    shutdown: {skip: "does not forward command to primary shard"},
    split: {skip: "does not forward command to primary shard"},
    splitVector: {skip: "does not forward command to primary shard"},
    startRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
    startSession: {skip: "executes locally on mongos (not sent to any remote node)"},
    stopRecordingTraffic: {skip: "executes locally on mongos (not sent to any remote node)"},
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
            }
        }
    },
    updateRole: {skip: "always targets the config server"},
    updateUser: {skip: "always targets the config server"},
    updateZoneKeyRange: {skip: "not on a user database"},
    usersInfo: {skip: "always targets the config server"},
    validate: {
        run: {
            sendsDbVersion: true,
            setUp: function(mongosConn, dbName, collName) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: function(dbName, collName) {
                return {validate: collName};
            },
            cleanUp: function(mongosConn, dbName, collName) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
};

commandsRemovedFromMongosIn44.forEach(function(cmd) {
    testCases[cmd] = {skip: "must define test coverage for 4.2 backwards compatibility"};
});

const st = new ShardingTest({shards: 2, mongos: 2});
const unshardedCollName = "foo";
const shardedCollName = "bar";
const sendTestDbName = "sendVersions";
const checkTestDbName = "checkVersions";

let res = st.s0.adminCommand({listCommands: 1});
assert.commandWorked(res);

let primaryShard = st.shard0;
let otherShard = st.shard1;
assert.commandWorked(st.s0.adminCommand({enableSharding: sendTestDbName}));
st.ensurePrimaryShard(sendTestDbName, primaryShard.shardName);

// Create a sharded collection that does not get moved during movePrimary.
let shardedCollNs = sendTestDbName + "." + shardedCollName;

assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard0.shardName, zone: 'x < 0'}));
assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard1.shardName, zone: 'x >= 0'}));

assert.commandWorked(st.s0.adminCommand(
    {updateZoneKeyRange: shardedCollNs, min: {x: MinKey}, max: {x: 0}, zone: 'x < 0'}));
assert.commandWorked(st.s0.adminCommand(
    {updateZoneKeyRange: shardedCollNs, min: {x: 0}, max: {x: MaxKey}, zone: 'x >= 0'}));

assert.commandWorked(
    st.s0.getDB('admin').admin.runCommand({shardCollection: shardedCollNs, key: {"x": 1}}));
assert.commandWorked(st.s0.getDB(sendTestDbName).createCollection(shardedCollName));

// Check that the shards has the unsharded collections.
assert(containsCollection(primaryShard, sendTestDbName, shardedCollName));
assert(containsCollection(otherShard, sendTestDbName, shardedCollName));

// Drop the database to create an entry for the database in the sharding catalog.
assert.commandWorked(st.s0.getDB(sendTestDbName).runCommand({drop: unshardedCollName}));
assert(containsDatabase(primaryShard, sendTestDbName));

let dbVersion =
    st.s0.getDB("config").getCollection("databases").findOne({_id: sendTestDbName}).version;
let routingInfo = {primaryShard: primaryShard, dbVersion: dbVersion};

// Check that the command was received with or without a databaseVersion as expected by the
// 'testCase' for the command.
for (let command of Object.keys(res.commands)) {
    let testCase = testCases[command];
    assert(testCase !== undefined, "coverage failure: must define a test case for " + command);
    if (!testCases[command].validated) {
        validateTestCase(testCase);
        testCases[command].validated = true;
    }

    if (testCase.skip) {
        print("skipping " + command + ": " + testCase.skip);
        continue;
    }

    runCommandTestCaseSendDbVersion(
        testCase.run, st, routingInfo, sendTestDbName, unshardedCollName);
    if (testCase.explain) {
        runCommandTestCaseSendDbVersion(
            testCase.explain, st, routingInfo, sendTestDbName, unshardedCollName);
    }
}

// Test that commands that send databaseVersion are subjected to the databaseVersion check even if
// the database does not exist on targeted shard
for (let command of Object.keys(res.commands)) {
    let testCase = testCases[command];
    if (testCase.skip) {
        print("skipping " + command + ": " + testCase.skip);
        continue;
    }

    let dbName = checkTestDbName + "-" + command;
    runCommandTestCaseCheckDbVersion(testCase.run, st, dbName, unshardedCollName);
    if (testCase.explain) {
        dbName = checkTestDbName + "-explain-" + command;
        runCommandTestCaseCheckDbVersion(testCase.explain, st, dbName, unshardedCollName);
    }
}

// After iterating through all the existing commands, ensure there were no additional test cases
// that did not correspond to any mongos command.
for (let key of Object.keys(testCases)) {
    // We have defined real test cases for commands added in 4.4 so that the test cases are
    // exercised in the regular suites, but because these test cases can't run in the last stable
    // suite, we skip processing them here to avoid failing the below assertion. We have defined
    // "skip" test cases for commands removed in 4.4 so the test case is defined in last stable
    // suites (in which these commands still exist on the mongos), but these test cases won't be
    // run in regular suites, so we skip processing them below as well.
    if (commandsAddedToMongosIn44.includes(key) || commandsRemovedFromMongosIn44.includes(key)) {
        continue;
    }
    assert(testCases[key].validated || testCases[key].conditional,
           "you defined a test case for a command '" + key +
               "' that does not exist on mongos: " + tojson(testCases[key]));
}

st.stop();
})();
