/**
 * Tests that commands that can be sent to secondaries for sharded collections are "safe":
 * - the secondary participates in the shard versioning protocol
 * - the secondary filters returned documents using its routing table cache.
 *
 * Since some commands are unversioned even against primaries or cannot be run on sharded
 * collections, this file declaratively defines the expected behavior for each command.
 *
 * If versioned secondary reads do not apply to a command, it should specify "skip" with the reason.
 *
 * The following fields are required for each command that is not skipped:
 *
 * - setUp: A function that does any set up (inserts, etc.) needed to check the command's results.
 * - command: The command to run, with all required options. Note, this field is also used to
 *            identify the operation in the system profiler.
 * - checkResults: A function that asserts whether the command should succeed or fail. If the
 *                 command is expected to succeed, the function should assert the expected results
 *                 *when the the collection has been dropped and recreated as empty.*
 * - behavior: Must be one of "unshardedOnly", "unversioned", or "versioned". Determines what
 *             checks the test performs against the system profilers of the secondaries.
 */
(function() {
    "use strict";

    load('jstests/libs/profiler.js');

    let db = "test";
    let coll = "foo";
    let nss = db + "." + coll;

    // Given a command, build its expected shape in the system profiler.
    let buildCommandProfile = function(command) {
        let commandProfile = {ns: nss};
        for (let key in command) {
            commandProfile["command." + key] = command[key];
        }
        return commandProfile;
    };

    // Check that a test case is well-formed.
    let validateTestCase = function(test) {
        assert(test.setUp && typeof(test.setUp) === "function");
        assert(test.command && typeof(test.command) === "object");
        assert(test.checkResults && typeof(test.checkResults) === "function");
        assert(test.behavior === "unshardedOnly" || test.behavior === "unversioned" ||
               test.behavior === "versioned");
    };

    let testCases = {
        _configsvrAddShard: {skip: "primary only"},
        _configsvrAddShardToZone: {skip: "primary only"},
        _configsvrBalancerStart: {skip: "primary only"},
        _configsvrBalancerStatus: {skip: "primary only"},
        _configsvrBalancerStop: {skip: "primary only"},
        _configsvrCommitChunkMerge: {skip: "primary only"},
        _configsvrCommitChunkMigration: {skip: "primary only"},
        _configsvrCommitChunkSplit: {skip: "primary only"},
        _configsvrMoveChunk: {skip: "primary only"},
        _configsvrMovePrimary: {skip: "primary only"},
        _configsvrRemoveShardFromZone: {skip: "primary only"},
        _configsvrShardCollection: {skip: "primary only"},
        _configsvrSetFeatureCompatibilityVersion: {skip: "primary only"},
        _configsvrUpdateZoneKeyRange: {skip: "primary only"},
        _getUserCacheGeneration: {skip: "does not return user data"},
        _hashBSONElement: {skip: "does not return user data"},
        _isSelf: {skip: "does not return user data"},
        _mergeAuthzCollections: {skip: "primary only"},
        _migrateClone: {skip: "primary only"},
        _recvChunkAbort: {skip: "primary only"},
        _recvChunkCommit: {skip: "primary only"},
        _recvChunkStart: {skip: "primary only"},
        _recvChunkStatus: {skip: "primary only"},
        _transferMods: {skip: "primary only"},
        addShard: {skip: "primary only"},
        addShardToZone: {skip: "primary only"},
        aggregate: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {aggregate: coll, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}},
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(0, res.cursor.firstBatch.length, tojson(res));
            },
            behavior: "versioned"
        },
        appendOplogNote: {skip: "primary only"},
        applyOps: {skip: "primary only"},
        authSchemaUpgrade: {skip: "primary only"},
        authenticate: {skip: "does not return user data"},
        availableQueryOptions: {skip: "does not return user data"},
        balancerStart: {skip: "primary only"},
        balancerStatus: {skip: "primary only"},
        balancerStop: {skip: "primary only"},
        buildInfo: {skip: "does not return user data"},
        captrunc: {skip: "primary only"},
        checkShardingIndex: {skip: "primary only"},
        cleanupOrphaned: {skip: "primary only"},
        clearLog: {skip: "does not return user data"},
        clone: {skip: "primary only"},
        cloneCollection: {skip: "primary only"},
        cloneCollectionAsCapped: {skip: "primary only"},
        collMod: {skip: "primary only"},
        collStats: {skip: "does not return user data"},
        compact: {skip: "does not return user data"},
        configureFailPoint: {skip: "does not return user data"},
        connPoolStats: {skip: "does not return user data"},
        connPoolSync: {skip: "does not return user data"},
        connectionStatus: {skip: "does not return user data"},
        convertToCapped: {skip: "primary only"},
        copydb: {skip: "primary only"},
        copydbgetnonce: {skip: "primary only"},
        copydbsaslstart: {skip: "primary only"},
        count: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {count: coll, query: {x: 1}},
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(0, res.n, tojson(res));
            },
            behavior: "versioned"
        },
        cpuload: {skip: "does not return user data"},
        create: {skip: "primary only"},
        createIndexes: {skip: "primary only"},
        createRole: {skip: "primary only"},
        createUser: {skip: "primary only"},
        currentOp: {skip: "does not return user data"},
        dataSize: {skip: "does not return user data"},
        dbHash: {skip: "does not return user data"},
        dbStats: {skip: "does not return user data"},
        delete: {skip: "primary only"},
        diagLogging: {skip: "does not return user data"},
        distinct: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {distinct: coll, key: "x"},
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(0, res.values.length, tojson(res));
            },
            behavior: "unversioned"
        },
        driverOIDTest: {skip: "does not return user data"},
        drop: {skip: "primary only"},
        dropAllRolesFromDatabase: {skip: "primary only"},
        dropAllUsersFromDatabase: {skip: "primary only"},
        dropDatabase: {skip: "primary only"},
        dropIndexes: {skip: "primary only"},
        dropRole: {skip: "primary only"},
        dropUser: {skip: "primary only"},
        emptycapped: {skip: "primary only"},
        enableSharding: {skip: "primary only"},
        eval: {skip: "primary only"},
        explain: {skip: "TODO SERVER-30068"},
        features: {skip: "does not return user data"},
        filemd5: {skip: "does not return user data"},
        find: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {find: coll, filter: {x: 1}},
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(0, res.cursor.firstBatch.length, tojson(res));
            },
            behavior: "versioned"
        },
        findAndModify: {skip: "primary only"},
        flushRouterConfig: {skip: "does not return user data"},
        forceerror: {skip: "does not return user data"},
        forceRoutingTableRefresh: {skip: "does not return user data"},
        fsync: {skip: "does not return user data"},
        fsyncUnlock: {skip: "does not return user data"},
        geoNear: {
            setUp: function(mongosConn) {
                assert.commandWorked(mongosConn.getCollection(nss).runCommand(
                    {createIndexes: coll, indexes: [{key: {loc: "2d"}, name: "loc_2d"}]}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1, loc: [1, 1]}));
            },
            command: {geoNear: coll, near: [1, 1]},
            checkResults: function(res) {
                // The command should fail on the new collection, because the geo index was dropped.
                assert.commandFailed(res);
            },
            behavior: "unversioned"
        },
        geoSearch: {skip: "not supported in mongos"},
        getCmdLineOpts: {skip: "does not return user data"},
        getDiagnosticData: {skip: "does not return user data"},
        getLastError: {skip: "primary only"},
        getLog: {skip: "does not return user data"},
        getMore: {skip: "shard version already established"},
        getParameter: {skip: "does not return user data"},
        getPrevError: {skip: "does not return user data"},
        getShardMap: {skip: "does not return user data"},
        getShardVersion: {skip: "primary only"},
        getnonce: {skip: "does not return user data"},
        godinsert: {skip: "for testing only"},
        grantPrivilegesToRole: {skip: "primary only"},
        grantRolesToRole: {skip: "primary only"},
        grantRolesToUser: {skip: "primary only"},
        group: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1, y: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1, y: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 2, y: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 2, y: 1}));
            },
            command: {group: {ns: coll, key: {x: 1}}},
            checkResults: function(res) {
                // Expect the command to fail, since it cannot run on sharded collections.
                assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation, tojson(res));
            },
            behavior: "unshardedOnly"
        },
        handshake: {skip: "does not return user data"},
        hostInfo: {skip: "does not return user data"},
        insert: {skip: "primary only"},
        invalidateUserCache: {skip: "does not return user data"},
        isdbgrid: {skip: "does not return user data"},
        isMaster: {skip: "does not return user data"},
        journalLatencyTest: {skip: "does not return user data"},
        killCursors: {skip: "does not return user data"},
        killOp: {skip: "does not return user data"},
        listCollections: {skip: "primary only"},
        listCommands: {skip: "does not return user data"},
        listDatabases: {skip: "primary only"},
        listIndexes: {skip: "primary only"},
        listShards: {skip: "does not return user data"},
        lockInfo: {skip: "primary only"},
        logApplicationMessage: {skip: "primary only"},
        logRotate: {skip: "does not return user data"},
        logout: {skip: "does not return user data"},
        makeSnapshot: {skip: "does not return user data"},
        mapReduce: {skip: "TODO SERVER-30068"},
        mergeChunks: {skip: "primary only"},
        moveChunk: {skip: "primary only"},
        movePrimary: {skip: "primary only"},
        netstat: {skip: "does not return user data"},
        parallelCollectionScan: {skip: "is an internal command"},
        ping: {skip: "does not return user data"},
        planCacheClear: {skip: "does not return user data"},
        planCacheClearFilters: {skip: "does not return user data"},
        planCacheListFilters: {skip: "does not return user data"},
        planCacheListPlans: {skip: "does not return user data"},
        planCacheListQueryShapes: {skip: "does not return user data"},
        planCacheSetFilter: {skip: "does not return user data"},
        profile: {skip: "primary only"},
        reIndex: {skip: "does not return user data"},
        removeShard: {skip: "primary only"},
        removeShardFromZone: {skip: "primary only"},
        renameCollection: {skip: "primary only"},
        repairCursor: {skip: "does not return user data"},
        repairDatabase: {skip: "does not return user data"},
        replSetAbortPrimaryCatchUp: {skip: "does not return user data"},
        replSetElect: {skip: "does not return user data"},
        replSetFreeze: {skip: "does not return user data"},
        replSetFresh: {skip: "does not return user data"},
        replSetGetConfig: {skip: "does not return user data"},
        replSetGetRBID: {skip: "does not return user data"},
        replSetGetStatus: {skip: "does not return user data"},
        replSetHeartbeat: {skip: "does not return user data"},
        replSetInitiate: {skip: "does not return user data"},
        replSetMaintenance: {skip: "does not return user data"},
        replSetReconfig: {skip: "does not return user data"},
        replSetRequestVotes: {skip: "does not return user data"},
        replSetStepDown: {skip: "does not return user data"},
        replSetStepUp: {skip: "does not return user data"},
        replSetSyncFrom: {skip: "does not return user data"},
        replSetTest: {skip: "does not return user data"},
        replSetUpdatePosition: {skip: "does not return user data"},
        replSetResizeOplog: {skip: "does not return user data"},
        resetError: {skip: "does not return user data"},
        resync: {skip: "primary only"},
        revokePrivilegesFromRole: {skip: "primary only"},
        revokeRolesFromRole: {skip: "primary only"},
        revokeRolesFromUser: {skip: "primary only"},
        rolesInfo: {skip: "primary only"},
        saslContinue: {skip: "primary only"},
        saslStart: {skip: "primary only"},
        serverStatus: {skip: "does not return user data"},
        setCommittedSnapshot: {skip: "does not return user data"},
        setFeatureCompatibilityVersion: {skip: "primary only"},
        setParameter: {skip: "does not return user data"},
        setShardVersion: {skip: "does not return user data"},
        shardCollection: {skip: "primary only"},
        shardConnPoolStats: {skip: "does not return user data"},
        shardingState: {skip: "does not return user data"},
        shutdown: {skip: "does not return user data"},
        sleep: {skip: "does not return user data"},
        split: {skip: "primary only"},
        splitChunk: {skip: "primary only"},
        splitVector: {skip: "primary only"},
        stageDebug: {skip: "primary only"},
        startSession: {skip: "does not return user data"},
        top: {skip: "does not return user data"},
        touch: {skip: "does not return user data"},
        unsetSharding: {skip: "does not return user data"},
        update: {skip: "primary only"},
        updateRole: {skip: "primary only"},
        updateUser: {skip: "primary only"},
        updateZoneKeyRange: {skip: "primary only"},
        usersInfo: {skip: "primary only"},
        validate: {skip: "does not return user data"},
        whatsmyuri: {skip: "does not return user data"}
    };

    let scenarios = {
        dropRecreateAsUnshardedOnSameShard: function(
            staleMongos, freshMongos, test, commandProfile) {
            let primaryShardSecondary = st.rs0.getSecondary();

            // Drop and recreate the collection.
            assert.commandWorked(freshMongos.getDB(db).runCommand({drop: coll}));
            assert.commandWorked(freshMongos.getDB(db).runCommand({create: coll}));

            let res = staleMongos.getDB(db).runCommand(
                Object.assign({}, test.command, {$readPreference: {mode: 'secondary'}}));

            test.checkResults(res);

            if (test.behavior === "unshardedOnly") {
                profilerDoesNotHaveMatchingEntryOrThrow(primaryShardSecondary.getDB(db),
                                                        commandProfile);
            } else if (test.behavior === "unversioned") {
                // Check that the primary shard secondary received the request *without* an
                // attached shardVersion and returned success.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": false},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            } else if (test.behavior == "versioned") {
                // Check that the primary shard secondary returned stale shardVersion.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": ErrorCodes.SendStaleConfig
                    },
                                  commandProfile));

                // Check that the primary shard secondary received the request again and returned
                // success.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            }
        },
        dropRecreateAsShardedOnSameShard: function(staleMongos, freshMongos, test, commandProfile) {
            let primaryShardSecondary = st.rs0.getSecondary();

            // Drop and recreate the collection as sharded.
            assert.commandWorked(freshMongos.getDB(db).runCommand({drop: coll}));
            assert.commandWorked(freshMongos.getDB(db).runCommand({create: coll}));
            assert.commandWorked(freshMongos.adminCommand({shardCollection: nss, key: {x: 1}}));

            let res = staleMongos.getDB(db).runCommand(
                Object.assign({}, test.command, {$readPreference: {mode: 'secondary'}}));

            test.checkResults(res);

            if (test.behavior === "unshardedOnly") {
                profilerDoesNotHaveMatchingEntryOrThrow(primaryShardSecondary.getDB(db),
                                                        commandProfile);
            } else if (test.behavior === "unversioned") {
                // Check that the primary shard secondary received the request *without* an
                // attached shardVersion and returned success.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": false},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            } else if (test.behavior == "versioned") {
                // Check that the primary shard secondary returned stale shardVersion.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": ErrorCodes.SendStaleConfig
                    },
                                  commandProfile));

                // Check that the primary shard secondary received the request again and returned
                // success.
                profilerHasSingleMatchingEntryOrThrow(
                    primaryShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            }
        },
        dropRecreateAsUnshardedOnDifferentShard: function(
            staleMongos, freshMongos, test, commandProfile) {
            // There is no way to drop and recreate the collection as unsharded on a *different*
            // shard without calling movePrimary, and it is known that a stale mongos will not
            // refresh its notion of the primary shard after it loads it once.
        },
        dropRecreateAsShardedOnDifferentShard: function(
            staleMongos, freshMongos, test, commandProfile) {
            let donorShardSecondary = st.rs0.getSecondary();
            let recipientShardSecondary = st.rs1.getSecondary();

            // Drop and recreate the collection as sharded, and move the chunk to the other shard.
            assert.commandWorked(freshMongos.getDB(db).runCommand({drop: coll}));
            assert.commandWorked(freshMongos.getDB(db).runCommand({create: coll}));
            assert.commandWorked(freshMongos.adminCommand({shardCollection: nss, key: {x: 1}}));
            assert.commandWorked(freshMongos.adminCommand({
                moveChunk: nss,
                find: {x: 0},
                to: st.shard1.shardName,
            }));

            let res = staleMongos.getDB(db).runCommand(
                Object.assign({}, test.command, {$readPreference: {mode: 'secondary'}}));

            test.checkResults(res);

            if (test.behavior === "unshardedOnly") {
                profilerDoesNotHaveMatchingEntryOrThrow(donorShardSecondary.getDB(db),
                                                        commandProfile);
                profilerDoesNotHaveMatchingEntryOrThrow(recipientShardSecondary.getDB(db),
                                                        commandProfile);
            } else if (test.behavior === "unversioned") {
                // Check that the donor shard secondary received the request *without* an attached
                // shardVersion and returned success.
                profilerHasSingleMatchingEntryOrThrow(
                    donorShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": false},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            } else if (test.behavior == "versioned") {
                // Check that the donor shard secondary returned stale shardVersion.
                profilerHasSingleMatchingEntryOrThrow(
                    donorShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": ErrorCodes.SendStaleConfig
                    },
                                  commandProfile));

                // Check that the recipient shard secondary received the request and returned
                // success.
                profilerHasSingleMatchingEntryOrThrow(
                    recipientShardSecondary.getDB(db),
                    Object.extend({
                        "command.shardVersion": {"$exists": true},
                        "command.$readPreference": {"mode": "secondary"},
                        "exceptionCode": {"$exists": false}
                    },
                                  commandProfile));
            }
        }
    };

    // Set the secondaries to priority 0 and votes 0 to prevent the primaries from stepping down.
    let rsOpts = {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]};
    let st = new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts}});

    let freshMongos = st.s0;
    let staleMongos = st.s1;

    let res = st.s.adminCommand({listCommands: 1});
    assert.commandWorked(res);

    let commands = Object.keys(res.commands);
    for (let command of commands) {
        let test = testCases[command];
        assert(test !== undefined,
               "coverage failure: must define a safe secondary reads test for " + command);

        if (test.skip !== undefined) {
            print("skipping " + command + ": " + test.skip);
            continue;
        }
        validateTestCase(test);

        // Build the query to identify the operation in the system profiler.
        let commandProfile = buildCommandProfile(test.command);

        for (let scenario in scenarios) {
            jsTest.log("testing command " + tojson(command) + " under scenario " + scenario);

            // Each scenario starts with a sharded collection with shard0 as the primary shard.
            assert.commandWorked(staleMongos.adminCommand({enableSharding: db}));
            st.ensurePrimaryShard(db, st.shard0.shardName);
            assert.commandWorked(staleMongos.adminCommand({shardCollection: nss, key: {x: 1}}));

            // Do any test-specific setup.
            test.setUp(staleMongos);

            // Turn on system profiler on both secondaries.
            assert.commandWorked(st.rs0.getSecondary().getDB(db).setProfilingLevel(2));
            assert.commandWorked(st.rs1.getSecondary().getDB(db).setProfilingLevel(2));

            // Do dummy read from the stale mongos so it loads the routing table into memory once.
            assert.commandWorked(staleMongos.getDB(db).runCommand({find: coll}));

            scenarios[scenario](staleMongos, freshMongos, test, commandProfile);

            // Clean up the database by dropping it; this is the only way to drop the profiler
            // collection on secondaries.
            // Do this from staleMongos, so staleMongos purges the database entry from its cache.
            assert.commandWorked(staleMongos.getDB(db).runCommand({dropDatabase: 1}));
        }
    }

    st.stop();
})();
