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
 *                 command is expected to succeed, the function should assert the expected results.
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
        _getUserCacheGeneration: {skip: "no user data returned"},
        _hashBSONElement: {skip: "no user data returned"},
        _isSelf: {skip: "no user data returned"},
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
                // The command should work and return correct results.
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
            },
            behavior: "versioned"
        },
        appendOplogNote: {skip: "primary only"},
        applyOps: {skip: "primary only"},
        authSchemaUpgrade: {skip: "primary only"},
        authenticate: {skip: "no user data returned"},
        availableQueryOptions: {skip: "no user data returned"},
        balancerStart: {skip: "primary only"},
        balancerStatus: {skip: "primary only"},
        balancerStop: {skip: "primary only"},
        buildInfo: {skip: "no user data returned"},
        captrunc: {skip: "primary only"},
        checkShardingIndex: {skip: "primary only"},
        cleanupOrphaned: {skip: "primary only"},
        clearLog: {skip: "no user data returned"},
        clone: {skip: "primary only"},
        cloneCollection: {skip: "primary only"},
        cloneCollectionAsCapped: {skip: "primary only"},
        collMod: {skip: "primary only"},
        collStats: {skip: "no user data returned"},
        compact: {skip: "no user data returned"},
        configureFailPoint: {skip: "no user data returned"},
        connPoolStats: {skip: "no user data returned"},
        connPoolSync: {skip: "no user data returned"},
        connectionStatus: {skip: "no user data returned"},
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
                // The command should work and return correct results.
                assert.commandWorked(res);
                assert.eq(1, res.n, tojson(res));
            },
            behavior: "versioned"
        },
        cpuload: {skip: "no user data returned"},
        create: {skip: "primary only"},
        createIndexes: {skip: "primary only"},
        createRole: {skip: "primary only"},
        createUser: {skip: "primary only"},
        currentOp: {skip: "no user data returned"},
        dataSize: {skip: "no user data returned"},
        dbHash: {skip: "no user data returned"},
        dbStats: {skip: "no user data returned"},
        delete: {skip: "primary only"},
        diagLogging: {skip: "no user data returned"},
        distinct: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {distinct: coll, key: "x"},
            checkResults: function(res) {
                assert.commandWorked(res);
                // Expect the command not to find any results, since the chunk moved.
                assert.eq(0, res.values.length, tojson(res));
            },
            behavior: "unversioned"
        },
        driverOIDTest: {skip: "no user data returned"},
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
        features: {skip: "no user data returned"},
        filemd5: {skip: "no user data returned"},
        find: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {find: coll, filter: {x: 1}},
            checkResults: function(res) {
                // The command should work and return correct results.
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
            },
            behavior: "versioned"
        },
        findAndModify: {skip: "primary only"},
        flushRouterConfig: {skip: "no user data returned"},
        forceerror: {skip: "no user data returned"},
        forceRoutingTableRefresh: {skip: "no user data returned"},
        fsync: {skip: "no user data returned"},
        fsyncUnlock: {skip: "no user data returned"},
        geoNear: {
            setUp: function(mongosConn) {
                assert.commandWorked(mongosConn.getCollection(nss).runCommand(
                    {createIndexes: coll, indexes: [{key: {loc: "2d"}, name: "loc_2d"}]}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1, loc: [1, 1]}));
            },
            command: {geoNear: coll, near: [1, 1]},
            checkResults: function(res) {
                assert.commandWorked(res);
                // Expect the command not to find any results, since the chunk moved.
                assert.eq(0, res.results.length, res);
            },
            behavior: "unversioned"
        },
        geoSearch: {skip: "not supported in mongos"},
        getCmdLineOpts: {skip: "no user data returned"},
        getDiagnosticData: {skip: "no user data returned"},
        getLastError: {skip: "primary only"},
        getLog: {skip: "no user data returned"},
        getMore: {skip: "shard version already established"},
        getParameter: {skip: "no user data returned"},
        getPrevError: {skip: "no user data returned"},
        getShardMap: {skip: "no user data returned"},
        getShardVersion: {skip: "primary only"},
        getnonce: {skip: "no user data returned"},
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
        handshake: {skip: "no user data returned"},
        hostInfo: {skip: "no user data returned"},
        insert: {skip: "primary only"},
        invalidateUserCache: {skip: "no user data returned"},
        isdbgrid: {skip: "no user data returned"},
        isMaster: {skip: "no user data returned"},
        journalLatencyTest: {skip: "no user data returned"},
        killCursors: {skip: "no user data returned"},
        killOp: {skip: "no user data returned"},
        listCollections: {skip: "primary only"},
        listCommands: {skip: "no user data returned"},
        listDatabases: {skip: "primary only"},
        listIndexes: {skip: "primary only"},
        listShards: {skip: "no user data returned"},
        lockInfo: {skip: "primary only"},
        logApplicationMessage: {skip: "primary only"},
        logRotate: {skip: "no user data returned"},
        logout: {skip: "no user data returned"},
        makeSnapshot: {skip: "no user data returned"},
        mapReduce: {skip: "TODO SERVER-30068"},
        mergeChunks: {skip: "primary only"},
        moveChunk: {skip: "primary only"},
        movePrimary: {skip: "primary only"},
        netstat: {skip: "no user data returned"},
        parallelCollectionScan: {skip: "is an internal command"},
        ping: {skip: "no user data returned"},
        planCacheClear: {skip: "no user data returned"},
        planCacheClearFilters: {skip: "no user data returned"},
        planCacheListFilters: {skip: "no user data returned"},
        planCacheListPlans: {skip: "no user data returned"},
        planCacheListQueryShapes: {skip: "no user data returned"},
        planCacheSetFilter: {skip: "no user data returned"},
        profile: {skip: "primary only"},
        reIndex: {skip: "no user data returned"},
        removeShard: {skip: "primary only"},
        removeShardFromZone: {skip: "primary only"},
        renameCollection: {skip: "primary only"},
        repairCursor: {skip: "no user data returned"},
        repairDatabase: {skip: "no user data returned"},
        replSetAbortPrimaryCatchUp: {skip: "no user data returned"},
        replSetElect: {skip: "no user data returned"},
        replSetFreeze: {skip: "no user data returned"},
        replSetFresh: {skip: "no user data returned"},
        replSetGetConfig: {skip: "no user data returned"},
        replSetGetRBID: {skip: "no user data returned"},
        replSetGetStatus: {skip: "no user data returned"},
        replSetHeartbeat: {skip: "no user data returned"},
        replSetInitiate: {skip: "no user data returned"},
        replSetMaintenance: {skip: "no user data returned"},
        replSetReconfig: {skip: "no user data returned"},
        replSetRequestVotes: {skip: "no user data returned"},
        replSetStepDown: {skip: "no user data returned"},
        replSetStepUp: {skip: "no user data returned"},
        replSetSyncFrom: {skip: "no user data returned"},
        replSetTest: {skip: "no user data returned"},
        replSetUpdatePosition: {skip: "no user data returned"},
        replSetResizeOplog: {skip: "no user data returned"},
        resetError: {skip: "no user data returned"},
        resync: {skip: "primary only"},
        revokePrivilegesFromRole: {skip: "primary only"},
        revokeRolesFromRole: {skip: "primary only"},
        revokeRolesFromUser: {skip: "primary only"},
        rolesInfo: {skip: "primary only"},
        saslContinue: {skip: "primary only"},
        saslStart: {skip: "primary only"},
        serverStatus: {skip: "no user data returned"},
        setCommittedSnapshot: {skip: "no user data returned"},
        setFeatureCompatibilityVersion: {skip: "primary only"},
        setParameter: {skip: "no user data returned"},
        setShardVersion: {skip: "no user data returned"},
        shardCollection: {skip: "primary only"},
        shardConnPoolStats: {skip: "no user data returned"},
        shardingState: {skip: "no user data returned"},
        shutdown: {skip: "no user data returned"},
        sleep: {skip: "no user data returned"},
        split: {skip: "primary only"},
        splitChunk: {skip: "primary only"},
        splitVector: {skip: "primary only"},
        stageDebug: {skip: "primary only"},
        startSession: {skip: "no user data returned"},
        top: {skip: "no user data returned"},
        touch: {skip: "no user data returned"},
        unsetSharding: {skip: "no user data returned"},
        update: {skip: "primary only"},
        updateRole: {skip: "primary only"},
        updateUser: {skip: "primary only"},
        updateZoneKeyRange: {skip: "primary only"},
        usersInfo: {skip: "primary only"},
        validate: {skip: "no user data returned"},
        whatsmyuri: {skip: "no user data returned"}
    };

    // Set the secondaries to priority 0 and votes 0 to prevent the primaries from stepping down.
    let rsOpts = {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]};
    let st = new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts}});

    let donorShardSecondary = st.rs0.getSecondary();
    let recipientShardSecondary = st.rs1.getSecondary();

    let freshMongos = st.s0;
    let staleMongos = st.s1;

    assert.commandWorked(staleMongos.adminCommand({enableSharding: db}));
    st.ensurePrimaryShard(db, st.shard0.shardName);

    // Turn on system profiler on secondaries to collect data on all database operations.
    assert.commandWorked(donorShardSecondary.getDB(db).setProfilingLevel(2));
    assert.commandWorked(recipientShardSecondary.getDB(db).setProfilingLevel(2));

    let res = st.s.adminCommand({listCommands: 1});
    assert.commandWorked(res);

    let commands = Object.keys(res.commands);
    for (let command of commands) {
        let test = testCases[command];
        assert(test !== undefined,
               "coverage failure: must define a safe secondary reads test for " + command);

        if (test.skip !== undefined) {
            print("skipping " + test.command + ": " + test.skip);
            continue;
        }
        validateTestCase(test);

        jsTest.log("testing command " + tojson(test.command));

        assert.commandWorked(staleMongos.adminCommand({shardCollection: nss, key: {x: 1}}));
        assert.commandWorked(staleMongos.adminCommand({split: nss, middle: {x: 0}}));

        // Do dummy read from the stale mongos so that it loads the routing table into memory once.
        assert.commandWorked(staleMongos.getDB(db).runCommand({find: coll}));

        // Do any test-specific setup.
        test.setUp(staleMongos);

        // Do a moveChunk from the fresh mongos to make the other mongos stale.
        assert.commandWorked(freshMongos.adminCommand({
            moveChunk: nss,
            find: {x: 0},
            to: st.shard1.shardName,
            waitForDelete: true,
        }));

        let res = staleMongos.getDB(db).runCommand(
            Object.extend(test.command, {$readPreference: {mode: 'secondary'}}));

        test.checkResults(res);

        // Build the query to identify the operation in the system profiler.
        let commandProfile = buildCommandProfile(test.command);

        if (test.behavior === "unshardedOnly") {
            // Check that neither the donor shard secondary nor recipient shard secondary
            // received the request.
            profilerDoesNotHaveMatchingEntryOrThrow(donorShardSecondary.getDB(db), commandProfile);
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

            // Check that the recipient shard secondary did not receive the request.
            profilerDoesNotHaveMatchingEntryOrThrow(recipientShardSecondary.getDB(db),
                                                    commandProfile);
        } else if (test.behavior === "versioned") {
            // Check that the donor shard secondary returned stale shardVersion.
            profilerHasSingleMatchingEntryOrThrow(
                donorShardSecondary.getDB(db),
                Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "exceptionCode": ErrorCodes.SendStaleConfig
                },
                              commandProfile));

            // Check that the recipient shard secondary received the request and returned stale
            // shardVersion once, even though the mongos is fresh, because the secondary was
            // stale.
            profilerHasSingleMatchingEntryOrThrow(
                donorShardSecondary.getDB(db),
                Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "exceptionCode": ErrorCodes.SendStaleConfig
                },
                              commandProfile));

            // Check that the recipient shard secondary received the request again and returned
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

        // Clean up the collection by dropping it. This also drops all associated indexes.
        assert.commandWorked(freshMongos.getDB(db).runCommand({drop: coll}));
    }

    st.stop();
})();
