/**
 * Tests that commands that can be sent to secondaries for sharded collections can be "safe":
 * - When non-'available' read concern is specified (local in this case), the secondary participates
 *   in the shard versioning protocol and filters returned documents using its routing table cache.
 * - When 'available' read concern is specified, the secondary does not check shard version nor
 *   filters results.
 * - When no read concern is specified, the secondary defaults to 'available' read concern.
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
 *                 *when the range has not been deleted from the donor.*
 * - checkAvailableReadConcernResults: Same as checkResults above, except asserts the expected
 *                                     results for the command run with read concern 'available'.
 * - behavior: Must be one of "unshardedOnly", "targetsPrimaryUsesConnectionVersioning" or
 * "versioned". Determines what system profiler checks are performed.
 */
(function() {
    "use strict";

    load('jstests/libs/profiler.js');

    let db = "test";
    let coll = "foo";
    let nss = db + "." + coll;

    // Check that a test case is well-formed.
    let validateTestCase = function(test) {
        assert(test.setUp && typeof(test.setUp) === "function");
        assert(test.command && typeof(test.command) === "object");
        assert(test.checkResults && typeof(test.checkResults) === "function");
        assert(test.checkAvailableReadConcernResults &&
               typeof(test.checkAvailableReadConcernResults) === "function");
        assert(test.behavior === "unshardedOnly" ||
               test.behavior === "targetsPrimaryUsesConnectionVersioning" ||
               test.behavior === "versioned");
    };

    let testCases = {
        _addShard: {skip: "primary only"},
        _cloneCatalogData: {skip: "primary only"},
        _configsvrAddShard: {skip: "primary only"},
        _configsvrAddShardToZone: {skip: "primary only"},
        _configsvrBalancerStart: {skip: "primary only"},
        _configsvrBalancerStatus: {skip: "primary only"},
        _configsvrBalancerStop: {skip: "primary only"},
        _configsvrCommitChunkMerge: {skip: "primary only"},
        _configsvrCommitChunkMigration: {skip: "primary only"},
        _configsvrCommitChunkSplit: {skip: "primary only"},
        _configsvrCommitMovePrimary: {skip: "primary only"},
        _configsvrDropCollection: {skip: "primary only"},
        _configsvrDropDatabase: {skip: "primary only"},
        _configsvrMoveChunk: {skip: "primary only"},
        _configsvrMovePrimary: {skip: "primary only"},
        _configsvrRemoveShardFromZone: {skip: "primary only"},
        _configsvrShardCollection: {skip: "primary only"},
        _configsvrUpdateZoneKeyRange: {skip: "primary only"},
        _flushRoutingTableCacheUpdates: {skip: "does not return user data"},
        _getUserCacheGeneration: {skip: "does not return user data"},
        _hashBSONElement: {skip: "does not return user data"},
        _isSelf: {skip: "does not return user data"},
        _mergeAuthzCollections: {skip: "primary only"},
        _migrateClone: {skip: "primary only"},
        _movePrimary: {skip: "primary only"},
        _recvChunkAbort: {skip: "primary only"},
        _recvChunkCommit: {skip: "primary only"},
        _recvChunkStart: {skip: "primary only"},
        _recvChunkStatus: {skip: "primary only"},
        _transferMods: {skip: "primary only"},
        abortTransaction: {skip: "primary only"},
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
            checkAvailableReadConcernResults: function(res) {
                // The command should work and return orphaned results.
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
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
        commitTransaction: {skip: "primary only"},
        collMod: {skip: "primary only"},
        collStats: {skip: "does not return user data"},
        compact: {skip: "does not return user data"},
        configureFailPoint: {skip: "does not return user data"},
        connPoolStats: {skip: "does not return user data"},
        connPoolSync: {skip: "does not return user data"},
        connectionStatus: {skip: "does not return user data"},
        convertToCapped: {skip: "primary only"},
        copydb: {skip: "primary only"},
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
            checkAvailableReadConcernResults: function(res) {
                // The command should work and return orphaned results.
                assert.commandWorked(res);
                assert.eq(1, res.n, tojson(res));
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
        distinct: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {distinct: coll, key: "x"},
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(1, res.values.length, tojson(res));
            },
            checkAvailableReadConcernResults: function(res) {
                // The command should work and return orphaned results.
                assert.commandWorked(res);
                assert.eq(1, res.values.length, tojson(res));
            },
            behavior: "versioned"
        },
        driverOIDTest: {skip: "does not return user data"},
        drop: {skip: "primary only"},
        dropAllRolesFromDatabase: {skip: "primary only"},
        dropAllUsersFromDatabase: {skip: "primary only"},
        dropDatabase: {skip: "primary only"},
        dropIndexes: {skip: "primary only"},
        dropRole: {skip: "primary only"},
        dropUser: {skip: "primary only"},
        echo: {skip: "does not return user data"},
        emptycapped: {skip: "primary only"},
        enableSharding: {skip: "primary only"},
        endSessions: {skip: "does not return user data"},
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
                // The command should work and return correct results.
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
            },
            checkAvailableReadConcernResults: function(res) {
                // The command should work and return orphaned results.
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
            },
            behavior: "versioned"
        },
        findAndModify: {skip: "primary only"},
        flushRouterConfig: {skip: "does not return user data"},
        forceerror: {skip: "does not return user data"},
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
                // The command should work and return orphaned results, because it doesn't do
                // filtering and also because the collection is sharded, it will get broadcast to
                // both shards.
                assert.commandWorked(res);
                assert.eq(2, res.results.length, tojson(res));
            },
            checkAvailableReadConcernResults: function(res) {
                // The command should work and return orphaned results, because it doesn't do
                // filtering. The expected result is 1, because the stale mongos assumes the
                // collection is still on only 1 shard and will not broadcast it to both.
                assert.commandWorked(res);
                assert.eq(1, res.results.length, tojson(res));
            },
            behavior: "versioned"
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
        handshake: {skip: "does not return user data"},
        hostInfo: {skip: "does not return user data"},
        insert: {skip: "primary only"},
        invalidateUserCache: {skip: "does not return user data"},
        isdbgrid: {skip: "does not return user data"},
        isMaster: {skip: "does not return user data"},
        journalLatencyTest: {skip: "does not return user data"},
        killCursors: {skip: "does not return user data"},
        killAllSessions: {skip: "does not return user data"},
        killAllSessionsByPattern: {skip: "does not return user data"},
        killOp: {skip: "does not return user data"},
        killSessions: {skip: "does not return user data"},
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
        mapReduce: {
            setUp: function(mongosConn) {
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
                assert.writeOK(mongosConn.getCollection(nss).insert({x: 1}));
            },
            command: {
                mapReduce: coll,
                map: function() {
                    emit(this.x, 1);
                },
                reduce: function(key, values) {
                    return Array.sum(values);
                },
                out: {inline: 1}
            },
            checkResults: function(res) {
                assert.commandWorked(res);
                assert.eq(1, res.results.length, tojson(res));
                assert.eq(1, res.results[0]._id, tojson(res));
                assert.eq(2, res.results[0].value, tojson(res));
            },
            checkAvailableReadConcernResults: function(res) {
                assert.commandFailed(res);
            },
            behavior: "targetsPrimaryUsesConnectionVersioning"
        },
        mergeChunks: {skip: "primary only"},
        moveChunk: {skip: "primary only"},
        movePrimary: {skip: "primary only"},
        multicast: {skip: "does not return user data"},
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
        reapLogicalSessionCacheNow: {skip: "does not return user data"},
        refreshLogicalSessionCacheNow: {skip: "does not return user data"},
        refreshSessions: {skip: "does not return user data"},
        refreshSessionsInternal: {skip: "does not return user data"},
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
        restartCatalog: {skip: "internal-only command"},
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
        setFreeMonitoring: {skip: "primary only"},
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

    // Set the secondaries to priority 0 and votes 0 to prevent the primaries from stepping down.
    let rsOpts = {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]};
    let st = new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts}});

    let donorShardPrimary = st.rs0.getPrimary();
    let recipientShardPrimary = st.rs1.getPrimary();
    let donorShardSecondary = st.rs0.getSecondary();
    let recipientShardSecondary = st.rs1.getSecondary();

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

        jsTest.log("testing command " + tojson(test.command));

        assert.commandWorked(freshMongos.adminCommand({enableSharding: db}));
        st.ensurePrimaryShard(db, st.shard0.shardName);
        assert.commandWorked(freshMongos.adminCommand({shardCollection: nss, key: {x: 1}}));
        assert.commandWorked(freshMongos.adminCommand({split: nss, middle: {x: 0}}));

        // Do dummy read from the stale mongos so it loads the routing table into memory once.
        // Additionally, do a secondary read to ensure that the secondary has loaded the initial
        // routing table -- the first read to the primary will refresh the mongos' shardVersion,
        // which will then be used against the secondary to ensure the secondary is fresh.
        assert.commandWorked(staleMongos.getDB(db).runCommand({find: coll}));
        assert.commandWorked(freshMongos.getDB(db).runCommand(
            {find: coll, $readPreference: {mode: 'secondary'}, readConcern: {'level': 'local'}}));

        // Do any test-specific setup.
        test.setUp(staleMongos);

        assert.commandWorked(recipientShardPrimary.getDB(db).setProfilingLevel(2));
        assert.commandWorked(donorShardSecondary.getDB(db).setProfilingLevel(2));
        assert.commandWorked(recipientShardSecondary.getDB(db).setProfilingLevel(2));

        // Suspend range deletion on the donor shard.
        donorShardPrimary.adminCommand(
            {configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});

        // Do a moveChunk from the fresh mongos to make the other mongos stale.
        // Use {w:2} (all) write concern so the metadata change gets persisted to the secondary
        // before stalely versioned commands are sent against the secondary.
        assert.commandWorked(freshMongos.adminCommand({
            moveChunk: nss,
            find: {x: 0},
            to: st.shard1.shardName,
            _secondaryThrottle: true,
            writeConcern: {w: 2},
        }));

        let cmdReadPrefSecondary =
            Object.assign({}, test.command, {$readPreference: {mode: 'secondary'}});
        let cmdPrefSecondaryConcernAvailable =
            Object.assign({}, cmdReadPrefSecondary, {readConcern: {level: 'available'}});
        let cmdPrefSecondaryConcernLocal =
            Object.assign({}, cmdReadPrefSecondary, {readConcern: {level: 'local'}});

        let availableReadConcernRes =
            staleMongos.getDB(db).runCommand(cmdPrefSecondaryConcernAvailable);
        test.checkAvailableReadConcernResults(availableReadConcernRes);

        let defaultReadConcernRes = staleMongos.getDB(db).runCommand(cmdReadPrefSecondary);
        if (command === 'mapReduce') {
            // mapReduce is always sent to a primary, which defaults to 'local' readConcern
            test.checkResults(defaultReadConcernRes);
        } else {
            // Secondaries default to the 'available' readConcern
            test.checkAvailableReadConcernResults(defaultReadConcernRes);
        }

        let localReadConcernRes = staleMongos.getDB(db).runCommand(cmdPrefSecondaryConcernLocal);
        test.checkResults(localReadConcernRes);

        // Build the query to identify the operation in the system profiler.
        let commandProfile = buildCommandProfile(test.command, true /* sharded */);

        if (test.behavior === "unshardedOnly") {
            // Check that neither the donor nor recipient shard secondaries received either request.
            profilerHasZeroMatchingEntriesOrThrow(
                {profileDB: donorShardSecondary.getDB(db), filter: commandProfile});
            profilerHasZeroMatchingEntriesOrThrow(
                {profileDB: recipientShardSecondary.getDB(db), filter: commandProfile});
        } else if (test.behavior === "targetsPrimaryUsesConnectionVersioning") {
            // Check that the recipient shard primary received the request without a shardVersion
            // field and returned success.
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: recipientShardPrimary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": false},
                    "command.$readPreference": {$exists: false},
                    "command.readConcern": {"level": "local"},
                    "errCode": {"$exists": false},
                },
                                      commandProfile)
            });
        } else if (test.behavior === "versioned") {
            // Check that the donor shard secondary received both the 'available' read concern
            // request and read concern not specified request and returned success for both, despite
            // the mongos' stale routing table.
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: donorShardSecondary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "command.readConcern": {"level": "available"},
                    "errCode": {"$ne": ErrorCodes.StaleConfig},
                },
                                      commandProfile)
            });
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: donorShardSecondary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "command.readConcern": {"$exists": false},
                    "errCode": {"$ne": ErrorCodes.StaleConfig},
                },
                                      commandProfile)
            });

            // Check that the donor shard secondary then returned stale shardVersion for the request
            // with local read concern.
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: donorShardSecondary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "command.readConcern": {"level": "local"},
                    "errCode": ErrorCodes.StaleConfig
                },
                                      commandProfile)
            });

            // Check that the recipient shard secondary received the request with local read concern
            // and also returned stale shardVersion once, even though the mongos is fresh, because
            // the secondary was stale.
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: recipientShardSecondary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "command.readConcern": {"level": "local"},
                    "errCode": ErrorCodes.StaleConfig
                },
                                      commandProfile)
            });

            // Check that the recipient shard secondary received the request with local read concern
            // again and finally returned success.
            profilerHasSingleMatchingEntryOrThrow({
                profileDB: recipientShardSecondary.getDB(db),
                filter: Object.extend({
                    "command.shardVersion": {"$exists": true},
                    "command.$readPreference": {"mode": "secondary"},
                    "command.readConcern": {"level": "local"},
                    "errCode": {"$ne": ErrorCodes.StaleConfig},
                },
                                      commandProfile)
            });
        }

        donorShardPrimary.adminCommand({configureFailPoint: 'suspendRangeDeletion', mode: 'off'});

        // Clean up the collection by dropping the DB. This also drops all associated indexes and
        // clears the profiler collection.
        // Do this from staleMongos, so staleMongos purges the database entry from its cache.
        assert.commandWorked(staleMongos.getDB(db).runCommand({dropDatabase: 1}));
    }

    st.stop();
})();
