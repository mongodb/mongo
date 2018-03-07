/**
 * Specifies for each command whether it is expected to send a databaseVersion and shardVersion, and
 * verifies that the commands match the specification.
 */
(function() {
    'use strict';

    load('jstests/libs/profiler.js');

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    const SHARD_VERSION_UNSHARDED = [Timestamp(0, 0), ObjectId("000000000000000000000000")];

    function validateTestCase(testCase) {
        assert(testCase.skip || testCase.command,
               "must specify exactly one of 'skip' or 'command' for test case " + tojson(testCase));

        if (testCase.skip) {
            for (let key of Object.keys(testCase)) {
                assert(
                    key === "skip" || key === "enterpriseOnly",
                    "if a test case specifies 'skip', it must not specify any other fields besides 'enterpriseOnly': " +
                        key + ": " + tojson(testCase));
            }
            return;
        }

        // Check that required fields are present.
        assert(testCase.hasOwnProperty("sendsDbVersion"),
               "must specify 'sendsDbVersion' for test case " + tojson(testCase));
        assert(testCase.hasOwnProperty("sendsShardVersion"),
               "must specify 'sendsShardVersion' for test case " + tojson(testCase));

        // Check that all present fields are of the correct type.
        assert(typeof(testCase.command) === "object");
        assert(typeof(testCase.sendsDbVersion) === "boolean");
        assert(typeof(testCase.sendsShardVersion) === "boolean");
        assert(testCase.setUp ? typeof(testCase.setUp) === "function" : true,
               "setUp must be a function: " + tojson(testCase));
        assert(testCase.cleanUp ? typeof(testCase.cleanUp) === "function" : true,
               "cleanUp must be a function: " + tojson(testCase));
    }

    let testCases = {
        _hashBSONElement: {skip: "executes locally on mongos (not sent to any remote node)"},
        _isSelf: {skip: "executes locally on mongos (not sent to any remote node)"},
        _mergeAuthzCollections: {skip: "always targets the config server"},
        addShard: {skip: "not on a user database"},
        addShardToZone: {skip: "not on a user database"},
        aggregate: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            command: {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}},
        },
        authenticate: {skip: "does not forward command to primary shard"},
        availableQueryOptions: {skip: "executes locally on mongos (not sent to any remote node)"},
        balancerStart: {skip: "not on a user database"},
        balancerStatus: {skip: "not on a user database"},
        balancerStop: {skip: "not on a user database"},
        buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
        clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
        collMod: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {collMod: collName, noPadding: false},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        collStats: {
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
        },
        compact: {skip: "not allowed through mongos"},
        configureFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
        connPoolStats: {skip: "executes locally on mongos (not sent to any remote node)"},
        connPoolSync: {skip: "executes locally on mongos (not sent to any remote node)"},
        connectionStatus: {skip: "executes locally on mongos (not sent to any remote node)"},
        convertToCapped: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {convertToCapped: collName, size: 8192},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        copydb:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        count: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {count: collName, query: {x: 1}},
        },
        create: {
            sendsDbVersion: false,
            // The collection doesn't exist yet, so no shardVersion is sent.
            sendsShardVersion: false,
            command: {create: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        createIndexes:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        createRole: {skip: "always targets the config server"},
        createUser: {skip: "always targets the config server"},
        currentOp: {skip: "not on a user database"},
        dataSize: {
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
        },
        dbStats: {
            sendsDbVersion: false,
            // dbStats is always broadcast to all shards
            sendsShardVersion: false,
            command: {dbStats: 1, scale: 1}
        },
        delete: {
            sendsDbVersion: false,
            // The profiler extracts the individual deletes from the 'deletes' array, and so loses
            // the overall delete command's attached shardVersion, though one is sent.
            // The versioning for delete will be tested separately (TODO SERVER-33429).
            sendsShardVersion: false,
            command: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}
        },
        distinct: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {distinct: collName, key: "x"},
        },
        drop: {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        dropAllRolesFromDatabase: {skip: "always targets the config server"},
        dropAllUsersFromDatabase: {skip: "always targets the config server"},
        dropDatabase:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        dropIndexes: {
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
        },
        dropRole: {skip: "always targets the config server"},
        dropUser: {skip: "always targets the config server"},
        enableSharding: {skip: "does not forward command to primary shard"},
        endSessions: {skip: "goes through the cluster write path"},
        eval: {
            sendsDbVersion: false,
            // It is a known bug that eval does not send shardVersion (SERVER-33357).
            sendsShardVersion: false,
            command: {
                eval: function(collName) {
                    const doc = db[collName].findOne();
                },
                args: [collName]
            }
        },
        explain: {skip: "TODO SERVER-31226"},
        features: {skip: "executes locally on mongos (not sent to any remote node)"},
        filemd5: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            command: {filemd5: ObjectId(), root: collName}
        },
        find: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            command: {find: collName, filter: {x: 1}},
        },
        findAndModify: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            command: {findAndModify: collName, query: {_id: 0}, remove: true}
        },
        flushRouterConfig: {skip: "executes locally on mongos (not sent to any remote node)"},
        fsync: {skip: "broadcast to all shards"},
        geoNear: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist with a geo index, and does not implicitly create
                // the collection or index.
                assert.commandWorked(mongosConn.getCollection(ns).runCommand(
                    {createIndexes: collName, indexes: [{key: {loc: "2d"}, name: "loc_2d"}]}));
                assert.writeOK(mongosConn.getCollection(ns).insert({x: 1, loc: [1, 1]}));
            },
            command: {geoNear: collName, near: [1, 1]},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        getCmdLineOpts: {skip: "executes locally on mongos (not sent to any remote node)"},
        getDiagnosticData: {skip: "executes locally on mongos (not sent to any remote node)"},
        getLastError: {skip: "does not forward command to primary shard"},
        getLog: {skip: "executes locally on mongos (not sent to any remote node)"},
        getMore: {skip: "requires a previously established cursor"},
        getParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
        getPrevError: {skip: "not supported in mongos"},
        getShardMap: {skip: "executes locally on mongos (not sent to any remote node)"},
        getShardVersion: {skip: "executes locally on mongos (not sent to any remote node)"},
        getnonce: {skip: "not on a user database"},
        grantPrivilegesToRole: {skip: "always targets the config server"},
        grantRolesToRole: {skip: "always targets the config server"},
        grantRolesToUser: {skip: "always targets the config server"},
        group: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            command: {
                group:
                    {ns: collName, key: {x: 1}, $reduce: function(curr, result) {}, initial: {}}
            },
        },
        hostInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
        insert: {
            sendsDbVersion: false,
            sendsShardVersion: true,
            command: {insert: collName, documents: [{_id: 1}]},
            cleanUp: function(mongosConn) {
                // Implicitly creates the collection.
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
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
        listCollections:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        listCommands: {skip: "executes locally on mongos (not sent to any remote node)"},
        listDatabases: {skip: "does not forward command to primary shard"},
        listIndexes: {
            sendsDbVersion: false,
            // It's a known bug that listIndexes uses ShardConnection without connection versioning
            // (SERVER-33434).
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {listIndexes: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        listShards: {skip: "does not forward command to primary shard"},
        logApplicationMessage: {skip: "not on a user database", enterpriseOnly: true},
        logRotate: {skip: "executes locally on mongos (not sent to any remote node)"},
        logout: {skip: "not on a user database"},
        mapReduce: {
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
        },
        mergeChunks: {skip: "does not forward command to primary shard"},
        moveChunk: {skip: "does not forward command to primary shard"},
        movePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
        multicast: {skip: "does not forward command to primary shard"},
        netstat: {skip: "executes locally on mongos (not sent to any remote node)"},
        ping: {skip: "executes locally on mongos (not sent to any remote node)"},
        planCacheClear: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            command: {planCacheClear: collName}
        },
        planCacheClearFilters: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            command: {planCacheClearFilters: collName}
        },
        planCacheListFilters: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            command: {planCacheListFilters: collName}
        },
        planCacheListPlans: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            command: {planCacheListPlans: collName}
        },
        planCacheListQueryShapes: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            command: {planCacheListQueryShapes: collName}
        },
        planCacheSetFilter: {
            sendsDbVersion: false,
            // Uses connection versioning.
            sendsShardVersion: false,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        profile: {skip: "not supported in mongos"},
        reIndex: {
            sendsDbVersion: true,
            sendsShardVersion: true,
            setUp: function(mongosConn) {
                // Expects the collection to exist, and doesn't implicitly create it.
                assert.commandWorked(mongosConn.getDB(dbName).runCommand({create: collName}));
            },
            command: {reIndex: collName},
            cleanUp: function(mongosConn) {
                assert(mongosConn.getDB(dbName).getCollection(collName).drop());
            }
        },
        reapLogicalSessionCacheNow: {skip: "is a no-op on mongos"},
        refreshLogicalSessionCacheNow: {skip: "goes through the cluster write path"},
        refreshSessions: {skip: "executes locally on mongos (not sent to any remote node)"},
        refreshSessionsInternal: {skip: "executes locally on mongos (not sent to any remote node)"},
        removeShard: {skip: "not on a user database"},
        removeShardFromZone: {skip: "not on a user database"},
        renameCollection:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
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
        setFeatureCompatibilityVersion: {skip: "not on a user database"},
        setParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
        shardCollection: {skip: "does not forward command to primary shard"},
        shardConnPoolStats: {skip: "does not forward command to primary shard"},
        shutdown: {skip: "does not forward command to primary shard"},
        split: {skip: "does not forward command to primary shard"},
        splitVector: {skip: "does not forward command to primary shard"},
        startSession: {skip: "executes locally on mongos (not sent to any remote node)"},
        update: {
            sendsDbVersion: false,
            // The profiler extracts the individual updates from the 'updates' array, and so loses
            // the overall update command's attached shardVersion, though one is sent.
            // The versioning for update will be tested separately (TODO SERVER-33429).
            sendsShardVersion: false,
            command: {
                update: collName,
                updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}]
            }
        },
        updateRole: {skip: "always targets the config server"},
        updateUser: {skip: "always targets the config server"},
        updateZoneKeyRange: {skip: "not on a user database"},
        usersInfo: {skip: "always targets the config server"},
        validate:
            {skip: "Not captured by the profiler; will be tested separately (TODO SERVER-33429)"},
        whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
    };

    var st = new ShardingTest({shards: 1});

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    let dbVersion = st.s.getDB("config").getCollection("databases").findOne({_id: dbName}).version;
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    st.shard0.getDB(dbName).setProfilingLevel(2);

    let res = st.s.adminCommand({listCommands: 1});
    assert.commandWorked(res);
    let commands = Object.keys(res.commands);

    // Use the profiler to check that the command was received with or without a databaseVersion and
    // shardVersion as expected by the 'testCase' for the command.
    for (let command of commands) {
        let testCase = testCases[command];
        assert(testCase !== undefined, "coverage failure: must define a test case for " + command);
        validateTestCase(testCase);
        testCases[command].validated = true;

        if (testCase.skip) {
            print("skipping " + command + ": " + testCase.skip);
            continue;
        }

        // Drop the profiler collection before each command, so in case this test case fails, it's
        // easy to find this command in the profiler output that gets logged.
        st.shard0.getDB(dbName).setProfilingLevel(0);
        assert(st.shard0.getDB(dbName).getCollection("system.profile").drop());
        st.shard0.getDB(dbName).setProfilingLevel(2);

        jsTest.log("testing command " + tojson(testCase.command));

        if (testCase.setUp) {
            testCase.setUp(st.s);
        }

        let commandProfile = buildCommandProfile(testCase.command, false);
        commandProfile["command.shardVersion"] =
            testCase.sendsShardVersion ? SHARD_VERSION_UNSHARDED : {$exists: false};

        st.shard0.adminCommand({configureFailPoint: "checkForDbVersionMismatch", mode: "alwaysOn"});
        if (testCase.sendsDbVersion) {
            commandProfile["command.databaseVersion"] = dbVersion;
            assert.commandFailedWithCode(st.s.getDB(dbName).runCommand(testCase.command),
                                         ErrorCodes.StaleDbVersion);

            // TODO: Currently, commands are profiled if they call CurOp::raiseDbProfilingLevel().
            // But, some commands do so only after calling AutoGetDb, where dbVersion is checked.
            // So, commands that send dbVersion will throw inside of AutoGetDb and may not be
            // profiled. SERVER-33499 will change the server so that CurOp::raiseDbProfilingLevel()
            // is called as part of generic command processing, before AutoGetDb can be called. Once
            // that is in, we should check that the dbVersion sent matched what was expected.
            // profilerHasSingleMatchingEntryOrThrow(
            //    {profileDB: st.shard0.getDB(dbName), filter: commandProfile});
        } else {
            commandProfile["command.databaseVersion"] = {$exists: false};
            assert.commandWorked(st.s.getDB(dbName).runCommand(testCase.command));
            profilerHasSingleMatchingEntryOrThrow(
                {profileDB: st.shard0.getDB(dbName), filter: commandProfile});
        }
        st.shard0.adminCommand({configureFailPoint: "checkForDbVersionMismatch", mode: "off"});

        if (testCase.cleanUp) {
            testCase.cleanUp(st.s);
        }
    }

    // After iterating through all the existing commands, ensure there were no additional test cases
    // that did not correspond to any mongos command.
    for (let key of Object.keys(testCases)) {
        assert(testCases[key].validated || testCases[key].enterpriseOnly,
               "you defined a test case for a command '" + key +
                   "' that does not exist on mongos: " + tojson(testCases[key]));
    }

    st.stop();

})();
