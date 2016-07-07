/*
 * Declaratively-defined tests for views for all database commands. This file contains a map of test
 * definitions as well as code to run them.
 *
 * The example test
 *
 *      {
 *          command: {insert: "view", documents: [{x: 1}]},
 *          expectFailure: true
 *      }
 *
 * invokes runCommand with `command` and expects it to fail with an error code specific to views.
 *
 * Each test takes the following options:
 *
 *  command
 *      The command object to pass to db.runCommand(). Each command can assume that there exists a
 *      view named "view", built on top of a collection named "collection".
 *
 *  runOnDb
 *      Specifies the database against which to run db.runCommand(). If not specified, defaults to
 *      "test".
 *
 *  skip
 *      A string that, if present, causes the test runner to skip running this command altogether.
 *      The value should be the reason why the test is being skipped. (There is a predefined
 *      selection of commonly-used reasons below.)
 *
 *  expectFailure
 *      If true, assert that the command fails with a views-specific error code. Otherwise, all
 *      commands are expected to succeed.
 *
 *  setup
 *      A function that will be run before the command is executed. It takes a handle to a
 *      connection object as its single argument.
 *
 *  teardown
 *      A function that will be run after the command is executed. It takes a handle to a connection
 *      object as its single argument.
 *
 *  skipSharded
 *      If true, do not run this command on a mongos.
 *
 *  skipStandalone
 *      If true, do not run this command on a standalone mongod.
 */

(function() {
    "use strict";

    // Pre-written reasons for skipping a test.
    const isAnInternalCommand = "internal command";
    const isUnrelated = "is unrelated";
    const needsToFailWithViewsErrorCode =
        "TODO(SERVER-24823): needs to fail with a views-specific error code";

    let viewsCommandTests = {
        _configsvrAddShard: {skip: isAnInternalCommand},
        _configsvrAddShardToZone: {skip: isAnInternalCommand},
        _configsvrBalancerStart: {skip: isAnInternalCommand},
        _configsvrBalancerStatus: {skip: isAnInternalCommand},
        _configsvrBalancerStop: {skip: isAnInternalCommand},
        _configsvrCommitChunkMigration: {skip: isAnInternalCommand},
        _configsvrMoveChunk: {skip: isAnInternalCommand},
        _configsvrRemoveShardFromZone: {skip: isAnInternalCommand},
        _getUserCacheGeneration: {skip: isAnInternalCommand},
        _hashBSONElement: {skip: isAnInternalCommand},
        _isSelf: {skip: isAnInternalCommand},
        _mergeAuthzCollections: {skip: isAnInternalCommand},
        _migrateClone: {skip: isAnInternalCommand},
        _recvChunkAbort: {skip: isAnInternalCommand},
        _recvChunkCommit: {skip: isAnInternalCommand},
        _recvChunkStart: {skip: isAnInternalCommand},
        _recvChunkStatus: {skip: isAnInternalCommand},
        _transferMods: {skip: isAnInternalCommand},
        aggregate: {command: {aggregate: "view", pipeline: [{$match: {}}]}},
        appendOplogNote: {skip: isUnrelated},
        applyOps: {
            command: {applyOps: [{op: "i", o: {_id: 1}, ns: "test.view"}]},
            expectFailure: true,
            skip: needsToFailWithViewsErrorCode
        },
        authSchemaUpgrade: {skip: isUnrelated},
        authenticate: {skip: isUnrelated},
        availableQueryOptions: {skip: isUnrelated},
        buildInfo: {skip: isUnrelated},
        captrunc: {
            command: {captrunc: "view", n: 2, inc: false},
            expectFailure: true,
            skip: needsToFailWithViewsErrorCode
        },
        checkShardingIndex: {skip: isUnrelated},
        cleanupOrphaned: {command: {cleanupOrphaned: 1}, skip: "TODO(SERVER-24764)"},
        clone: {skip: "TODO(SERVER-24506)"},
        cloneCollection:
            {command: {cloneCollection: "test.views"}, skip: needsToFailWithViewsErrorCode},
        cloneCollectionAsCapped: {
            command: {cloneCollectionAsCapped: "view", toCollection: "testcapped", size: 10240},
            expectFailure: true,
            skip: needsToFailWithViewsErrorCode
        },
        collMod: {command: {collMod: "view", viewOn: "other"}, skip: "TODO(SERVER-24766)"},
        collStats: {command: {collStats: "view"}, skip: "TODO(SERVER-24823)"},
        compact:
            {command: {compact: "view"}, expectFailure: true, skip: needsToFailWithViewsErrorCode},
        configureFailPoint: {skip: isUnrelated},
        connPoolStats: {skip: isUnrelated},
        connPoolSync: {skip: isUnrelated},
        connectionStatus: {skip: isUnrelated},
        convertToCapped: {command: {convertToCapped: "view"}, skip: needsToFailWithViewsErrorCode},
        copydb: {skip: "TODO(SERVER-24823)"},
        copydbgetnonce: {skip: isUnrelated},
        copydbsaslstart: {skip: isUnrelated},
        count: {skip: "TODO(SERVER-24766)"},
        create: {skip: "tested in views/views_creation.js"},
        createIndexes: {
            command: {createIndexes: "view", indexes: [{key: {x: 1}, name: "x_1"}]},
            expectFailure: true,
            skip: "TODO(SERVER-24823)"
        },
        createRole: {
            command: {createRole: "testrole", privileges: [], roles: []},
            setup: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        createUser: {
            command: {createUser: "testuser", pwd: "testpass", roles: []},
            setup: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllUsersFromDatabase: 1}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllUsersFromDatabase: 1}));
            }
        },
        currentOp: {skip: isUnrelated},
        currentOpCtx: {skip: isUnrelated},
        dataSize: {
            command: {dataSize: "test.view"},
            expectFailure: true,
        },
        dbHash: {command: {dbHash: 1}, skip: "TODO(SERVER-24766)"},
        dbStats: {command: {dbStats: 1}, skip: "TODO(SERVER-24568)"},
        delete: {command: {delete: "view", deletes: [{q: {x: 1}, limit: 1}]}, expectFailure: true},
        diagLogging: {skip: isUnrelated},
        distinct: {command: {distinct: "view", key: "x"}, expectFailure: true},
        driverOIDTest: {skip: isUnrelated},
        drop: {command: {drop: "view"}, skip: "TODO(SERVER-24766)"},
        dropAllRolesFromDatabase: {skip: isUnrelated},
        dropAllUsersFromDatabase: {skip: isUnrelated},
        dropDatabase: {command: {dropDatabase: 1}},
        dropIndexes: {command: {dropIndexes: "view"}, skip: "TODO(SERVER-24823)"},
        dropRole: {
            command: {dropRole: "testrole"},
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        dropUser: {skip: isUnrelated},
        emptycapped: {
            command: {emptycapped: "view"},
            expectFailure: true,
            skip: needsToFailWithViewsErrorCode
        },
        eval: {skip: "TODO(SERVER-24823)"},
        explain: {command: {explain: {count: "view"}}, expectFailure: true},
        features: {skip: isUnrelated},
        filemd5: {skip: isUnrelated},
        find: {command: {find: "view"}, expectFailure: true},
        findAndModify: {skip: "TODO(SERVER-24766)"},
        forceerror: {skip: isUnrelated},
        fsync: {skip: isUnrelated},
        fsyncUnlock: {skip: isUnrelated},
        geoNear: {skip: "TODO(SERVER-24766)"},
        geoSearch: {skip: "TODO(SERVER-24766)"},
        getCmdLineOpts: {skip: isUnrelated},
        getLastError: {skip: isUnrelated},
        getLog: {skip: isUnrelated},
        getMore: {skip: "TODO(SERVER-24724)"},
        getParameter: {skip: isUnrelated},
        getPrevError: {skip: isUnrelated},
        getShardMap: {skip: isUnrelated},
        getShardVersion: {
            command: {getShardVersion: "test.view"},
            runOnDb: "admin",
            expectFailure: true,
            skip: "TODO(SERVER-24764)"
        },
        getnonce: {skip: isUnrelated},
        godinsert: {skip: isAnInternalCommand},
        grantPrivilegesToRole: {
            command: {
                grantPrivilegesToRole: "testrole",
                privileges: [{resource: {db: "test", collection: "view"}, actions: ["find"]}]
            },
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        grantRolesToRole: {
            command: {grantRolesToRole: "testrole", roles: ["read"]},
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        grantRolesToUser: {
            command: {grantRolesToUser: "testuser", roles: ["read"]},
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createUser: "testuser", pwd: "testpass", roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllUsersFromDatabase: 1}));
            },
            skip: "TODO(SERVER-24724)"
        },
        group: {
            command: {group: {ns: "test.view", key: "x", $reduce: function() {}, initial: {}}},
            skip: needsToFailWithViewsErrorCode
        },
        handshake: {skip: isUnrelated},
        hostInfo: {skip: isUnrelated},
        insert: {skip: "tested in views/views_disallowed_crud_commands.js"},
        invalidateUserCache: {skip: isUnrelated},
        isMaster: {skip: isUnrelated},
        journalLatencyTest: {skip: isUnrelated},
        killCursors: {skip: "TODO(SERVER-24771)"},
        killOp: {skip: isUnrelated},
        listCollections: {skip: "tested in views/views_creation.js"},
        listCommands: {skip: isUnrelated},
        listDatabases: {skip: isUnrelated},
        listIndexes: {command: {listIndexes: "view"}, skip: "TODO(SERVER-24823)"},
        lockInfo: {skip: isUnrelated},
        logApplicationMessage: {skip: isUnrelated},
        logRotate: {skip: isUnrelated},
        logout: {skip: isUnrelated},
        makeSnapshot: {skip: isAnInternalCommand},
        mapReduce: {skip: "tested in views/disallowed_crud_commands.js"},
        "mapreduce.shardedfinish": {skip: isAnInternalCommand},
        mergeChunks: {
            command: {mergeChunks: "view", bounds: [{x: 0}, {x: 10}]},
            runOnDb: "admin",
            skip: needsToFailWithViewsErrorCode
        },
        moveChunk: {command: {moveChunk: 1}, skip: needsToFailWithViewsErrorCode},
        parallelCollectionScan: {command: {parallelCollectionScan: "view"}, expectFailure: true},
        ping: {command: {ping: 1}},
        planCacheClear: {command: {planCacheClear: "view"}, expectFailure: true},
        planCacheClearFilters: {command: {planCacheClearFilters: "view"}, expectFailure: true},
        planCacheListFilters: {command: {planCacheListFilters: "view"}, expectFailure: true},
        planCacheListPlans: {command: {planCacheListPlans: "view"}, expectFailure: true},
        planCacheListQueryShapes:
            {command: {planCacheListQueryShapes: "view"}, expectFailure: true},
        planCacheSetFilter: {command: {planCacheSetFilter: "view"}, expectFailure: true},
        profile: {skip: isUnrelated},
        reIndex: {command: {reIndex: "view"}, expectFailure: true, skip: "TODO(SERVER-24823)"},
        renameCollection: {
            command: {renameCollection: "test.view", to: "test.otherview"},
            runOnDb: "admin",
            skip: "TODO(SERVER-24823)"
        },
        repairCursor: {command: {repairCursor: "view"}, expectFailure: true},
        repairDatabase: {command: {repairDatabase: 1}},
        replSetElect: {skip: isUnrelated},
        replSetFreeze: {skip: isUnrelated},
        replSetFresh: {skip: isUnrelated},
        replSetGetConfig: {skip: isUnrelated},
        replSetGetRBID: {skip: isUnrelated},
        replSetGetStatus: {skip: isUnrelated},
        replSetHeartbeat: {skip: isUnrelated},
        replSetInitiate: {skip: isUnrelated},
        replSetMaintenance: {skip: isUnrelated},
        replSetReconfig: {skip: isUnrelated},
        replSetRequestVotes: {skip: isUnrelated},
        replSetStepDown: {skip: isUnrelated},
        replSetStepUp: {skip: isUnrelated},
        replSetSyncFrom: {skip: isUnrelated},
        replSetTest: {skip: isUnrelated},
        replSetUpdatePosition: {skip: isUnrelated},
        resetError: {skip: isUnrelated},
        resync: {skip: isUnrelated},
        revokePrivilegesFromRole: {
            command: {
                revokePrivilegesFromRole: "testrole",
                privileges: [{resource: {db: "test", collection: "view"}, actions: ["find"]}]
            },
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        revokeRolesFromRole: {skip: isUnrelated},
        revokeRolesFromUser: {skip: isUnrelated},
        rolesInfo: {skip: isUnrelated},
        saslContinue: {skip: isUnrelated},
        saslStart: {skip: isUnrelated},
        serverStatus: {command: {serverStatus: 1}, skip: "TODO(SERVER-24568)"},
        setCommittedSnapshot: {skip: isAnInternalCommand},
        setParameter: {skip: isUnrelated},
        setShardVersion: {skip: isUnrelated},
        shardConnPoolStats: {skip: isUnrelated},
        shardingState: {skip: isUnrelated},
        shutdown: {skip: isUnrelated},
        sleep: {skip: isUnrelated},
        splitChunk:
            {command: {splitChunk: 1}, expectFailure: true, skip: needsToFailWithViewsErrorCode},
        splitVector:
            {command: {splitVector: 1}, expectFailure: true, skip: needsToFailWithViewsErrorCode},
        stageDebug: {skip: isAnInternalCommand},
        top: {command: {top: "view"}, runOnDb: "admin", skip: "TODO(SERVER-24568)"},
        touch: {
            command: {touch: "view", data: true},
            expectFailure: true,
            skip: needsToFailWithViewsErrorCode
        },
        unsetSharding: {skip: isAnInternalCommand},
        update: {command: {update: "view", updates: [{q: {x: 1}, u: {x: 2}}]}, expectFailure: true},
        updateRole: {
            command: {
                updateRole: "testrole",
                privileges: [{resource: {db: "test", collection: "view"}, actions: ["find"]}]
            },
            setup: function(conn) {
                assert.commandWorked(
                    conn.runCommand({createRole: "testrole", privileges: [], roles: []}));
            },
            teardown: function(conn) {
                assert.commandWorked(conn.runCommand({dropAllRolesFromDatabase: 1}));
            }
        },
        updateUser: {skip: isUnrelated},
        usersInfo: {skip: isUnrelated},
        validate: {command: {validate: "view"}, skip: "TODO(SERVER-24823)"},
        whatsmyuri: {skip: isUnrelated}
    };

    /**
     * Helper function for failing commands or writes.
     */
    let assertCommandOrWriteFailed = function(res) {
        if (res.writeErrors !== undefined)
            assert.neq(0, res.writeErrors.length);
        else
            assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);
    };

    // Are we on a mongos?
    let dbgridRes = db.adminCommand({isdbgrid: 1});
    const isMongos = (dbgridRes.ok === 1 && dbgridRes.isdbgrid === 1);

    // Obtain a list of all commands.
    let res = db.runCommand({listCommands: 1});
    assert.commandWorked(res);

    let commands = Object.keys(res.commands);
    for (let command of commands) {
        let test = viewsCommandTests[command];
        assert(test !== undefined,
               "Coverage failure: must explicitly define a views test for " + command);

        // Tests can be explicitly skipped. Print the name of the skipped test, as well as the
        // reason why.
        if (test.skip !== undefined) {
            print("Skipping " + command + ": " + test.skip);
            continue;
        }

        let dbName = (test.runOnDb === undefined) ? "test" : test.runOnDb;
        let dbHandle = db.getSiblingDB(dbName);

        // Skip tests depending on sharding configuration.
        if (test.skipSharded && isMongos)
            continue;
        if (test.skipStandalone && !isMongos)
            continue;

        // Perform test setup, and call any additional setup callbacks provided by the test. All
        // tests assume that there exists a view named 'view' that is backed by 'collection'.
        assert.commandWorked(dbHandle.dropDatabase());
        assert.commandWorked(dbHandle.runCommand({create: "view", viewOn: "collection"}));
        assert.writeOK(dbHandle.collection.insert({x: 1}));
        if (test.setup !== undefined)
            test.setup(dbHandle);

        // Execute the command. The print statement is necessary because in the event of a failure,
        // there is no way to determine what command was actually running.
        print("Testing " + command);

        if (test.expectFailure)
            assertCommandOrWriteFailed(dbHandle.runCommand(test.command));
        else
            assert.commandWorked(dbHandle.runCommand(test.command));

        if (test.teardown !== undefined)
            test.teardown(dbHandle);
    }
}());
