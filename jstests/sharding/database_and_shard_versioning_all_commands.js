/**
 * Specifies for each command whether it is expected to send a databaseVersion and shardVersion, and
 * verifies that the commands match the specification.
 */
(function() {
'use strict';

load('jstests/libs/profiler.js');
load('jstests/sharding/libs/last_stable_mongos_commands.js');
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const SHARD_VERSION_UNSHARDED = [Timestamp(0, 0), ObjectId("000000000000000000000000")];

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
    assert(testCase.hasOwnProperty("sendsShardVersion"),
           "must specify 'sendsShardVersion' for test case " + tojson(testCase));

    // Check that all present fields are of the correct type.
    assert(typeof (testCase.command) === "object");
    assert(testCase.runsAgainstAdminDb ? typeof (testCase.runsAgainstAdminDb) === "boolean" : true);
    assert(testCase.skipProfilerCheck ? typeof (testCase.skipProfilerCheck) === "boolean" : true);
    assert(typeof (testCase.sendsDbVersion) === "boolean");
    assert(typeof (testCase.sendsShardVersion) === "boolean");
    assert(testCase.setUp ? typeof (testCase.setUp) === "function" : true,
           "setUp must be a function: " + tojson(testCase));
    assert(testCase.cleanUp ? typeof (testCase.cleanUp) === "function" : true,
           "cleanUp must be a function: " + tojson(testCase));
}

function runCommandTestCase(testCase, st, routingInfo) {
    jsTest.log("testing command " + tojson(testCase.command));

    if (testCase.setUp) {
        testCase.setUp(st.s);
    }

    routingInfo.primaryShard.getDB(dbName).setProfilingLevel(2);
    let commandProfile = buildCommandProfile(testCase.command, false);
    commandProfile["command.shardVersion"] =
        testCase.sendsShardVersion ? SHARD_VERSION_UNSHARDED : {$exists: false};

    if (testCase.runsAgainstAdminDb) {
        assert.commandWorked(st.s.adminCommand(testCase.command));
    } else {
        assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command));
    }

    if (testCase.sendsDbVersion) {
        assertSentDatabaseVersion(
            testCase, commandProfile, routingInfo.dbVersion, routingInfo.primaryShard);
    } else {
        assertDidNotSendDatabaseVersion(testCase, commandProfile, routingInfo.primaryShard);
    }

    if (testCase.cleanUp) {
        testCase.cleanUp(st.s);
    }

    // Clear the profiler collection in between testing each command.
    routingInfo.primaryShard.getDB(dbName).setProfilingLevel(0);
    assert(routingInfo.primaryShard.getDB(dbName).getCollection("system.profile").drop());

    // Ensure the primary shard's database entry is stale for the next command by changing the
    // primary shard (the recipient shard does not refresh until getting a request with the new
    // version).
    let fromShard = st.getPrimaryShard(dbName);
    let toShard = st.getOther(fromShard);

    routingInfo.primaryShard = toShard;
    const previousDbVersion = routingInfo.dbVersion;

    assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: toShard.name}));
    routingInfo.dbVersion =
        st.s.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    // The dbVersion should have changed due to the movePrimary operation.
    assert.eq(routingInfo.dbVersion.lastMod, previousDbVersion.lastMod + 1);

    // The fromShard should have cleared its in-memory database info.
    const res = fromShard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.eq({}, res.dbVersion);
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
            sendsShardVersion: true,
            command: {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}},
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {
                explain:
                    {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}}
            },
        }
    },
    authenticate: {skip: "does not forward command to primary shard"},
    availableQueryOptions: {skip: "executes locally on mongos (not sent to any remote node)"},
    balancerStart: {skip: "not on a user database"},
    balancerStatus: {skip: "not on a user database"},
    balancerStop: {skip: "not on a user database"},
    buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
    clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
    collMod: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {collMod: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    collStats: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {collStats: collName},
            cleanUp: function(mongosConn) {
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
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {convertToCapped: collName, size: 8192},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    count: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {count: collName, query: {x: 1}},
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {explain: {count: collName, query: {x: 1}}},
        }
    },
    create: {
        run: {
            sendsDbVersion: false,
            // The collection doesn't exist yet, so no shardVersion is sent.
            sendsShardVersion: false,
            command: {create: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    createIndexes: {
        run: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]},
            cleanUp: function(mongosConn) {
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
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {dataSize: ns},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    dbStats: {
        run: {
            sendsDbVersion: false,
            // dbStats is always broadcast to all shards
            sendsShardVersion: false,
            command: {dbStats: 1, scale: 1}
        }
    },
    delete: {
        run: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            // The profiler extracts the individual deletes from the 'deletes' array, and so loses
            // the overall delete command's attached shardVersion, though one is sent.
            sendsShardVersion: true,
            command: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            // The profiler extracts the individual deletes from the 'deletes' array, and so loses
            // the overall delete command's attached shardVersion, though one is sent.
            sendsShardVersion: true,
            command: {explain: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}}
        }
    },
    distinct: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {distinct: collName, key: "x"},
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {explain: {distinct: collName, key: "x"}},
        },
    },
    drop: {
        run: {
            skipProfilerCheck: true,
            sendsDbVersion: false,
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {drop: collName},
        }
    },
    dropAllRolesFromDatabase: {skip: "always targets the config server"},
    dropAllUsersFromDatabase: {skip: "always targets the config server"},
    dropConnections: {skip: "not on a user database"},
    dropDatabase: {skip: "drops the database from the cluster, changing the UUID"},
    dropIndexes: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {dropIndexes: collName, index: "*"},
            cleanUp: function(mongosConn) {
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
            sendsShardVersion: true,
            command: {filemd5: ObjectId(), root: collName}
        }
    },
    find: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {find: collName, filter: {x: 1}},
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {explain: {find: collName, filter: {x: 1}}},
        }
    },
    findAndModify: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {findAndModify: collName, query: {_id: 0}, remove: true}
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {explain: {findAndModify: collName, query: {_id: 0}, remove: true}}
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
            sendsShardVersion: true,
            command: {insert: collName, documents: [{_id: 1}]},
            cleanUp: function(mongosConn) {
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
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {listCollections: 1},
        }
    },
    listCommands: {skip: "executes locally on mongos (not sent to any remote node)"},
    listDatabases: {skip: "does not forward command to primary shard"},
    listIndexes: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {listIndexes: collName},
            cleanUp: function(mongosConn) {
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
            sendsDbVersion: false,
            // mapReduce uses connection versioning rather than sending shardVersion in the command.
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {
                mapReduce: collName,
                map: function() {
                    emit(this.x, 1);
                },
                reduce: function(key, values) {
                    return Array.sum(values);
                },
                out: {inline: 1}
            },
            cleanUp: function(mongosConn) {
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
    planCacheClear:
        {run: {sendsDbVersion: true, sendsShardVersion: true, command: {planCacheClear: collName}}},
    planCacheClearFilters: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {planCacheClearFilters: collName}
        }
    },
    planCacheListFilters: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {planCacheListFilters: collName}
        }
    },
    planCacheListPlans: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {planCacheListPlans: collName, query: {_id: "A"}},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        }
    },
    planCacheListQueryShapes: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {planCacheListQueryShapes: collName}
        }
    },
    planCacheSetFilter: {
        run: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]},
            cleanUp: function(mongosConn) {
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
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {
                renameCollection: dbName + "." + collName,
                to: dbName + "." + collName + "_renamed"
            },
            cleanUp: function(mongosConn) {
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
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command:
                {setIndexCommitQuorum: collName, indexNames: ["index"], commitQuorum: "majority"},
            cleanUp: function(mongosConn) {
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
            skipProfilerCheck: true,
            sendsDbVersion: true,
            // The profiler extracts the individual updates from the 'updates' array, and so loses
            // the overall update command's attached shardVersion, though one is sent.
            sendsShardVersion: true,
            command: {
                update: collName,
                updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
            }
        },
        explain: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            // The profiler extracts the individual updates from the 'updates' array, and so loses
            // the overall update command's attached shardVersion, though one is sent.
            sendsShardVersion: true,
            command: {
                explain: {
                    update: collName,
                    updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
                }
            }
        }
    },
    updateRole: {skip: "always targets the config server"},
    updateUser: {skip: "always targets the config server"},
    updateZoneKeyRange: {skip: "not on a user database"},
    usersInfo: {skip: "always targets the config server"},
    validate: {
        run: {
            skipProfilerCheck: true,
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {validate: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            },
        }
    },
    whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
};

commandsRemovedFromMongosIn44.forEach(function(cmd) {
    testCases[cmd] = {skip: "must define test coverage for 4.2 backwards compatibility"};
});

function assertSentDatabaseVersion(testCase, commandProfile, dbVersion, primaryShard) {
    const res = primaryShard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.eq(dbVersion, res.dbVersion);

    // If the test case is marked as not tracked by the profiler, then we won't be able to
    // verify the version was not sent here. Any test cases marked with this flag should be
    // fixed in SERVER-33499.
    if (!testCase.skipProfilerCheck) {
        commandProfile["command.databaseVersion"] = dbVersion;
        profilerHasSingleMatchingEntryOrThrow(
            {profileDB: primaryShard.getDB(dbName), filter: commandProfile});
    }
}

function assertDidNotSendDatabaseVersion(testCase, commandProfile, primaryShard) {
    const res = primaryShard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.eq({}, res.dbVersion);

    // If the test case is marked as not tracked by the profiler, then we won't be able to
    // verify the version was not sent here. Any test cases marked with this flag should be
    // fixed in SERVER-33499.
    if (!testCase.skipProfilerCheck) {
        commandProfile["command.databaseVersion"] = {$exists: false};
        profilerHasSingleMatchingEntryOrThrow(
            {profileDB: primaryShard.getDB(dbName), filter: commandProfile});
    }
}

const st = new ShardingTest({shards: 2});

// We do this create and drop so that we create an entry for the database in the sharding catalog.
assert.commandWorked(st.s.getDB(dbName).createCollection(collName));
assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collName}));

let primaryShard = st.shard0;
st.ensurePrimaryShard(dbName, primaryShard.shardName);

let dbVersion = st.s.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

let res = st.s.adminCommand({listCommands: 1});
assert.commandWorked(res);

let routingInfo = {primaryShard: primaryShard, dbVersion: dbVersion};

// Use the profiler to check that the command was received with or without a databaseVersion and
// shardVersion as expected by the 'testCase' for the command.
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
    runCommandTestCase(testCase.run, st, routingInfo);
    if (testCase.explain) {
        runCommandTestCase(testCase.explain, st, routingInfo);
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
