/**
 * Specifies for each command whether it is expected to send a databaseVersion, and verifies that
 * the commands match the specification.
 *
 * Each command must have exactly one corresponding test defined. Each defined test case must
 * correspond to an existing command. The allowable fields for the test cases are as follows:
 *
 *      - 'run': This is the specified test case that will be executed for each command.
 *      - 'skip': Use this field to skip the execution of the test case, along with a justification.
 *      It's important to note that this field doesn't bypass command validation; it only skips the
 *      actual run.
 *      - 'explain': This field is optional and is used to test the explain command on the specified
 *      test case.
 *      - 'conditional': If you set this field to true, the test case will skip the validation that
 *      ensures all test cases match existing commands. This is useful for commands that only exist
 *      in enterprise modules, for instance.
 *      - 'skipMultiversion': If you set this field to true, the test case will skip running in
 *      multiversion suites. This is useful if you have a command that existed behind a feature flag
 *      in the previous version and is now enabled.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    commandsAddedToMongodSinceLastLTS,
    commandsRemovedFromMongodSinceLastLTS,
} from "jstests/sharding/libs/last_lts_mongod_commands.js";
import {
    commandsAddedToMongosSinceLastLTS,
    commandsRemovedFromMongosSinceLastLTS,
} from "jstests/sharding/libs/last_lts_mongos_commands.js";

function getNewDbName(dbName) {
    if (!getNewDbName.counter) {
        getNewDbName.counter = 0;
    }
    getNewDbName.counter++;
    return "db" + getNewDbName.counter;
}

function assertMatchingDatabaseVersion(conn, dbName, dbVersion) {
    const res = conn.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.eq(dbVersion, res.dbVersion);
}

function containsCollection(shard, dbName, collName) {
    const res = shard.getDB(dbName).runCommand({listCollections: 1});
    assert.commandWorked(res);
    const collections = res.cursor.firstBatch;
    for (const collection of collections) {
        if (collection["name"] === collName) {
            return true;
        }
    }
    return false;
}

function toArray(what) {
    if (Array.isArray(what)) {
        return what;
    }
    return [what];
}

function validateTestCase(testCase, validateSendsDbVersion) {
    assert(
        testCase.skip || testCase.run,
        "must specify exactly one of 'skip' or 'run' for test case " + tojson(testCase),
    );

    if (testCase.skip) {
        for (const key of Object.keys(testCase)) {
            assert(
                key === "skip" || key === "conditional",
                "if a test case specifies 'skip', it must not specify any other fields besides 'conditional': " +
                    key +
                    ": " +
                    tojson(testCase),
            );
        }
        return;
    }

    for (const test of toArray(testCase.run)) {
        validateCommandTestCase(test, validateSendsDbVersion);
    }

    if (testCase.explain) {
        for (const test of toArray(testCase.explain)) {
            validateCommandTestCase(test, validateSendsDbVersion);
        }
    }
}

function validateCommandTestCase(testCase, validateSendsDbVersion) {
    assert(testCase.command, "must specify 'command' for test case " + tojson(testCase));

    if (validateSendsDbVersion) {
        // Check that required fields are present.
        assert(
            testCase.hasOwnProperty("sendsDbVersion"),
            "must specify 'sendsDbVersion' for test case " + tojson(testCase),
        );
    }

    // Check that all present fields are of the correct type.
    assert(typeof testCase.command === "function");
    assert(testCase.runsAgainstAdminDb ? typeof testCase.runsAgainstAdminDb === "boolean" : true);
    if (validateSendsDbVersion) {
        assert(typeof testCase.sendsDbVersion === "boolean");
    }
    assert(testCase.explicitlyCreateCollection ? typeof testCase.explicitlyCreateCollection === "boolean" : true);
    assert(testCase.expectNonEmptyCollection ? typeof testCase.expectNonEmptyCollection === "boolean" : true);
    assert(
        testCase.cleanUp ? typeof testCase.cleanUp === "function" : true,
        "cleanUp must be a function: " + tojson(testCase),
    );
}

function testCommandAfterMovePrimary(testCase, connection, st, dbName, collName) {
    const primaryShardBefore = st.getPrimaryShard(dbName);
    const primaryShardAfter = st.getOther(primaryShardBefore);
    const dbVersionBefore = st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    if (testCase.explicitlyCreateCollection) {
        assert.commandWorked(primaryShardBefore.getDB(dbName).runCommand({create: collName}));
    }
    if (testCase.expectNonEmptyCollection) {
        assert.commandWorked(primaryShardBefore.getDB(dbName).runCommand({insert: collName, documents: [{x: 0}]}));
    }

    // Ensure all nodes know the dbVersion before the movePrimary.
    assert.commandWorked(st.s0.adminCommand({flushRouterConfig: 1}));
    assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);

    if (!FeatureFlagUtil.isPresentAndEnabled(primaryShardBefore, "ShardAuthoritativeDbMetadataCRUD")) {
        assert.commandWorked(primaryShardBefore.adminCommand({_flushDatabaseCacheUpdates: dbName}));
    }
    if (!FeatureFlagUtil.isPresentAndEnabled(primaryShardAfter, "ShardAuthoritativeDbMetadataCRUD")) {
        assert.commandWorked(primaryShardAfter.adminCommand({_flushDatabaseCacheUpdates: dbName}));
    }

    assertMatchingDatabaseVersion(primaryShardBefore, dbName, dbVersionBefore);
    assertMatchingDatabaseVersion(primaryShardAfter, dbName, dbVersionBefore);

    // Run movePrimary through the second mongos.
    assert.commandWorked(st.s1.adminCommand({movePrimary: dbName, to: primaryShardAfter.name}));

    const dbVersionAfter = st.s1.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    // After the movePrimary, both old and new primary shards should have cleared the dbVersion.
    assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertMatchingDatabaseVersion(primaryShardBefore, dbName, {});
    assertMatchingDatabaseVersion(primaryShardAfter, dbName, {});

    const command = testCase.command(dbName, collName, dbVersionBefore, dbVersionAfter);
    jsTest.log(
        "testing command " +
            tojson(command) +
            " after movePrimary; primary shard before: " +
            primaryShardBefore +
            ", database version before: " +
            tojson(dbVersionBefore) +
            ", primary shard after: " +
            primaryShardAfter,
    );

    // Run the test case's command.
    const res = connection.getDB(testCase.runsAgainstAdminDb ? "admin" : dbName).runCommand(command);
    if (testCase.expectedFailureCode) {
        assert.commandFailedWithCode(res, testCase.expectedFailureCode);
    } else {
        assert.commandWorked(res);
    }

    // If this command does not go through the router then there is no need to check if they are
    // updated
    if (connection === st.s0 || connection === st.s1 || connection === st.s) {
        if (testCase.sendsDbVersion) {
            // If the command participates in database versioning, all nodes should now know the new
            // dbVersion:
            // 1. The mongos should have sent the stale dbVersion to the old primary shard
            // 2. The old primary shard should have returned StaleDbVersion and refreshed
            // 3. Which should have caused the mongos to refresh and retry against the new primary
            // shard
            // 4. The new primary shard should have returned StaleDbVersion and refreshed
            // 5. Which should have caused the mongos to refresh and retry again, this time
            // succeeding.
            assertMatchingDatabaseVersion(st.s0, dbName, dbVersionAfter);
            assertMatchingDatabaseVersion(primaryShardBefore, dbName, dbVersionAfter);
            assertMatchingDatabaseVersion(primaryShardAfter, dbName, dbVersionAfter);
        } else {
            // If the command does not participate in database versioning:
            // 1. The mongos should have targeted the old primary shard but not attached a dbVersion
            // 2. The old primary shard should have returned an ok response
            // 3. Both old and new primary shards should have cleared the dbVersion
            assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);
            assertMatchingDatabaseVersion(primaryShardBefore, dbName, {});
            assertMatchingDatabaseVersion(primaryShardAfter, dbName, {});
        }
    }

    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    } else {
        assert(st.s0.getDB(dbName).getCollection(collName).drop());
    }
}

function testCommandAfterDropRecreateDatabase(testCase, connection, st) {
    const dbName = getNewDbName();
    const collName = "foo";

    // Create the database by creating a collection in it.
    assert.commandWorked(st.s0.getDB(dbName).createCollection(collName));
    const dbVersionBefore = st.s0.getDB("config").getCollection("databases").findOne({_id: dbName}).version;
    const primaryShardBefore = st.getPrimaryShard(dbName);
    const primaryShardAfter = st.getOther(primaryShardBefore);

    // Ensure the router and primary shard know the dbVersion before the drop/recreate database.
    assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertMatchingDatabaseVersion(primaryShardBefore, dbName, dbVersionBefore);
    assertMatchingDatabaseVersion(primaryShardAfter, dbName, {});

    // Drop and recreate the database through the second mongos.
    assert.commandWorked(st.s1.getDB(dbName).dropDatabase());
    assert.commandWorked(st.s1.adminCommand({enableSharding: dbName, primaryShard: primaryShardAfter.shardName}));

    const dbVersionAfter = st.s1.getDB("config").getCollection("databases").findOne({_id: dbName}).version;

    if (testCase.explicitlyCreateCollection) {
        assert.commandWorked(primaryShardAfter.getDB(dbName).runCommand({create: collName}));
    }
    if (testCase.expectNonEmptyCollection) {
        assert.commandWorked(primaryShardAfter.getDB(dbName).runCommand({insert: collName, documents: [{x: 0}]}));
    }

    // The only change after the drop/recreate database should be that the old primary shard should
    // have cleared its dbVersion.
    assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);
    assertMatchingDatabaseVersion(primaryShardBefore, dbName, {});

    const command = testCase.command(dbName, collName, dbVersionBefore, dbVersionAfter);
    jsTest.log(
        "testing command " +
            tojson(command) +
            " after drop/recreate database; primary shard before: " +
            primaryShardBefore +
            ", database version before: " +
            tojson(dbVersionBefore) +
            ", primary shard after: " +
            primaryShardAfter,
    );

    // Run the test case's command.
    const res = connection.getDB(testCase.runsAgainstAdminDb ? "admin" : dbName).runCommand(command);
    if (testCase.expectedFailureCode) {
        assert.commandFailedWithCode(res, testCase.expectedFailureCode);
    } else {
        assert.commandWorked(res);
    }

    // If this command does not go through the router then there is no need to check if they are
    // updated
    if (connection === st.s0 || connection === st.s1 || connection === st.s) {
        if (testCase.sendsDbVersion) {
            // If the command participates in database versioning all nodes should now know the new
            // dbVersion:
            // 1. The mongos should have sent the stale dbVersion to the old primary shard
            // 2. The old primary shard should have returned StaleDbVersion and refreshed
            // 3. Which should have caused the mongos to refresh and retry against the new primary
            // shard
            // 4. The new primary shard should have returned StaleDbVersion and refreshed
            // 5. Which should have caused the mongos to refresh and retry again, this time
            // succeeding.
            assertMatchingDatabaseVersion(st.s0, dbName, dbVersionAfter);
            assertMatchingDatabaseVersion(primaryShardBefore, dbName, dbVersionAfter);
            assertMatchingDatabaseVersion(primaryShardAfter, dbName, dbVersionAfter);
        } else {
            // If the command does not participate in database versioning, none of the nodes' view
            // of the dbVersion should have changed:
            // 1. The mongos should have targeted the old primary shard but not attached a dbVersion
            // 2. The old primary shard should have returned an ok response
            assertMatchingDatabaseVersion(st.s0, dbName, dbVersionBefore);
            assertMatchingDatabaseVersion(primaryShardBefore, dbName, {});
        }
    }

    // Clean up.
    if (testCase.cleanUp) {
        testCase.cleanUp(st.s0, dbName, collName);
    } else {
        assert(st.s0.getDB(dbName).getCollection(collName).drop());
    }
    assert.commandWorked(st.s0.getDB(dbName).dropDatabase());
}

const allTestCases = {
    mongos: {
        _clusterQueryWithoutShardKey: {skip: "executed locally on a mongos (not sent to any remote node)"},
        _clusterWriteWithoutShardKey: {skip: "executed locally on a mongos (not sent to any remote node)"},
        _hashBSONElement: {skip: "executes locally on mongos (not sent to any remote node)"},
        _isSelf: {skip: "executes locally on mongos (not sent to any remote node)"},
        _killOperations: {skip: "executes locally on mongos (not sent to any remote node)"},
        _mergeAuthzCollections: {skip: "always targets the config server"},
        _mongotConnPoolStats: {skip: "not on a user database", conditional: true},
        _dropConnectionsToMongot: {skip: "not on a user database", conditional: true},
        _mirrorMaestroConnPoolStats: {skip: "not on a user database", conditional: true},
        _dropMirrorMaestroConnections: {skip: "not on a user database", conditional: true},
        abortMoveCollection: {skip: "always targets the config server"},
        abortReshardCollection: {skip: "always targets the config server"},
        abortTransaction: {skip: "unversioned and uses special targetting rules"},
        abortUnshardCollection: {skip: "always targets the config server"},
        addShard: {skip: "not on a user database"},
        addShardToZone: {skip: "not on a user database"},
        aggregate: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        aggregate: collName,
                        pipeline: [{$match: {x: 1}}],
                        cursor: {batchSize: 10},
                    };
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        explain: {
                            aggregate: collName,
                            pipeline: [{$match: {x: 1}}],
                            cursor: {batchSize: 10},
                        },
                    };
                },
            },
        },
        analyze: {
            skip: "unimplemented. Serves only as a stub.",
        }, // TODO SERVER-68055: Extend test to work with analyze
        analyzeShardKey: {
            run: {
                runsAgainstAdminDb: true,
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                expectNonEmptyCollection: true,
                // The command should fail while calculating the read and write distribution metrics
                // since the cardinality of the shard key is less than analyzeShardKeyNumRanges
                // which defaults to 100.
                expectedFailureCode: 4952606,
                command: function (dbName, collName) {
                    return {analyzeShardKey: dbName + "." + collName, key: {_id: 1}};
                },
            },
        },
        appendOplogNote: {skip: "unversioned and executes on all shards"},
        authenticate: {skip: "does not forward command to primary shard"},
        autoSplitVector: {skip: "does not forward command to primary shard"},
        balancerCollectionStatus: {skip: "does not forward command to primary shard"},
        balancerStart: {skip: "not on a user database"},
        balancerStatus: {skip: "not on a user database"},
        balancerStop: {skip: "not on a user database"},
        buildInfo: {skip: "executes locally on mongos (not sent to any remote node)"},
        bulkWrite: {
            run: {
                sendsDbVersion: true,
                runsAgainstAdminDb: true,
                command: function (dbName, collName) {
                    return {
                        bulkWrite: 1,
                        ops: [{insert: 0, document: {_id: 1}}],
                        nsInfo: [{ns: dbName + "." + collName}],
                    };
                },
            },
            skipMultiversion: true,
        },
        changePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
        checkMetadataConsistency: {
            run: {
                sendsDbVersion: true,
                runsAgainstAdminDb: false,
                command: function (dbName, collName) {
                    return {checkMetadataConsistency: 1};
                },
            },
        },
        cleanupReshardCollection: {skip: "always targets the config server"},
        cleanupStructuredEncryptionData: {skip: "requires encrypted collections"},
        clearJumboFlag: {skip: "does not forward command to primary shard"},
        clearLog: {skip: "executes locally on mongos (not sent to any remote node)"},
        collMod: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {collMod: collName};
                },
            },
        },
        collStats: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {collStats: collName};
                },
            },
        },
        commitReshardCollection: {skip: "always targets the config server"},
        commitShardRemoval: {skip: "not on a user database"},
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
                command: function (dbName, collName) {
                    return {convertToCapped: collName, size: 8192};
                },
            },
        },
        coordinateCommitTransaction: {skip: "unimplemented. Serves only as a stub."},
        count: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {count: collName, query: {x: 1}};
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {explain: {count: collName, query: {x: 1}}};
                },
            },
        },
        cpuload: {skip: "executes locally on mongos (not sent to any remote node)"},
        create: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {create: collName};
                },
            },
        },
        createIndexes: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {createIndexes: collName, indexes: [{key: {a: 1}, name: "index"}]};
                },
            },
        },
        createSearchIndexes: {skip: "executes locally on mongos", conditional: true},
        createRole: {skip: "always targets the config server"},
        createUnsplittableCollection: {
            skip: "Test command that which functionality will be integrated into createCollection",
        },
        createUser: {skip: "always targets the config server"},
        currentOp: {skip: "not on a user database"},
        dataSize: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {dataSize: dbName + "." + collName};
                },
            },
        },
        dbStats: {
            run: {
                // dbStats is always broadcast to all shards
                sendsDbVersion: false,
                command: function (dbName, collName) {
                    return {dbStats: 1, scale: 1};
                },
            },
        },
        delete: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]};
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {explain: {delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}};
                },
            },
        },
        distinct: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {distinct: collName, key: "x"};
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {explain: {distinct: collName, key: "x"}};
                },
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
                command: function (dbName, collName) {
                    return {dropIndexes: collName, index: "*"};
                },
            },
        },
        dropRole: {skip: "always targets the config server"},
        dropSearchIndex: {skip: "executes locally on mongos", conditional: true},
        dropUser: {skip: "always targets the config server"},
        echo: {skip: "does not forward command to primary shard"},
        enableSharding: {skip: "does not forward command to primary shard"},
        endSessions: {skip: "goes through the cluster write path"},
        explain: {skip: "already tested by each CRUD command through the 'explain' field"},
        features: {skip: "executes locally on mongos (not sent to any remote node)"},
        filemd5: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {filemd5: ObjectId(), root: collName};
                },
            },
        },
        find: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {find: collName, filter: {x: 1}};
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {explain: {find: collName, filter: {x: 1}}};
                },
            },
        },
        findAndModify: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {findAndModify: collName, query: {_id: 0}, remove: true};
                },
            },
            explain: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {explain: {findAndModify: collName, query: {_id: 0}, remove: true}};
                },
            },
        },
        flushRouterConfig: {skip: "executes locally on mongos (not sent to any remote node)"},
        fsync: {skip: "broadcast to all shards"},
        fsyncUnlock: {skip: "broadcast to all shards"},
        getAuditConfig: {skip: "not on a user database", conditional: true},
        getClusterParameter: {skip: "always targets the config server"},
        getCmdLineOpts: {skip: "executes locally on mongos (not sent to any remote node)"},
        getDatabaseVersion: {skip: "executes locally on mongos (not sent to any remote node)"},
        getDefaultRWConcern: {skip: "executes locally on mongos (not sent to any remote node)"},
        getDiagnosticData: {skip: "executes locally on mongos (not sent to any remote node)"},
        getLog: {skip: "executes locally on mongos (not sent to any remote node)"},
        getMore: {skip: "requires a previously established cursor"},
        getParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
        getQueryableEncryptionCountInfo: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        getQueryableEncryptionCountInfo: collName,
                        tokens: [
                            {
                                tokens: [
                                    {
                                        "s": BinData(0, "lUBO7Mov5Sb+c/D4cJ9whhhw/+PZFLCk/AQU2+BpumQ="),
                                    },
                                ],
                            },
                        ],
                        "queryType": "insert",
                    };
                },
            },
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
                command: function (dbName, collName) {
                    return {insert: collName, documents: [{_id: 1}]};
                },
            },
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
                command: function (dbName, collName) {
                    return {listCollections: 1};
                },
            },
        },
        listCommands: {skip: "executes locally on mongos (not sent to any remote node)"},
        listDatabases: {skip: "does not forward command to primary shard"},
        listIndexes: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {listIndexes: collName};
                },
            },
        },
        listSearchIndexes: {skip: "executes locally on mongos", conditional: true},
        listShards: {skip: "does not forward command to primary shard"},
        lockInfo: {skip: "not on a user database"},
        logApplicationMessage: {skip: "not on a user database", conditional: true},
        logMessage: {skip: "not on a user database"},
        logRotate: {skip: "executes locally on mongos (not sent to any remote node)"},
        logout: {skip: "not on a user database"},
        mapReduce: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        mapReduce: collName,
                        map: function mapFunc() {
                            emit(this.x, 1);
                        },
                        reduce: function reduceFunc(key, values) {
                            return Array.sum(values);
                        },
                        out: "inline",
                    };
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        explain: {
                            mapReduce: collName,
                            map: function mapFunc() {
                                emit(this.x, 1);
                            },
                            reduce: function reduceFunc(key, values) {
                                return Array.sum(values);
                            },
                            out: "inline",
                        },
                    };
                },
            },
        },
        mergeAllChunksOnShard: {skip: "does not forward command to primary shard"},
        mergeChunks: {skip: "does not forward command to primary shard"},
        moveChunk: {skip: "does not forward command to primary shard"},
        moveCollection: {skip: "does not forward command to primary shard"},
        movePrimary: {skip: "reads primary shard from sharding catalog with readConcern: local"},
        moveRange: {skip: "does not forward command to primary shard"},
        multicast: {skip: "does not forward command to primary shard"},
        netstat: {skip: "executes locally on mongos (not sent to any remote node)"},
        oidcListKeys: {skip: "executes locally on mongos (not sent to any remote node)", conditional: true},
        oidcRefreshKeys: {skip: "executes locally on mongos (not sent to any remote node)", conditional: true},
        ping: {skip: "executes locally on mongos (not sent to any remote node)"},
        planCacheClear: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {planCacheClear: collName};
                },
            },
        },
        planCacheClearFilters: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {planCacheClearFilters: collName};
                },
            },
        },
        planCacheListFilters: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {planCacheListFilters: collName};
                },
            },
        },
        planCacheSetFilter: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {planCacheSetFilter: collName, query: {_id: "A"}, indexes: [{_id: 1}]};
                },
            },
        },
        profile: {skip: "not supported in mongos"},
        reapLogicalSessionCacheNow: {skip: "is a no-op on mongos"},
        refineCollectionShardKey: {skip: "not on a user database"},
        refreshLogicalSessionCacheNow: {skip: "goes through the cluster write path"},
        refreshSessions: {skip: "executes locally on mongos (not sent to any remote node)"},
        releaseMemory: {skip: "requires a previously established cursor"},
        removeShard: {skip: "not on a user database"},
        removeShardFromZone: {skip: "not on a user database"},
        renameCollection: {
            run: {
                runsAgainstAdminDb: true,
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {
                        renameCollection: dbName + "." + collName,
                        to: dbName + "." + collName + "_renamed",
                    };
                },
                cleanUp: function (mongosConn, dbName, collName) {
                    assert(
                        mongosConn
                            .getDB(dbName)
                            .getCollection(collName + "_renamed")
                            .drop(),
                    );
                },
            },
        },
        repairShardedCollectionChunksHistory: {skip: "always targets the config server"},
        replicateSearchIndexCommand: {skip: "internal command for testing only"},
        replSetGetStatus: {skip: "not supported in mongos"},
        resetPlacementHistory: {skip: "always targets the config server"},
        reshardCollection: {skip: "does not forward command to primary shard"},
        revokePrivilegesFromRole: {skip: "always targets the config server"},
        revokeRolesFromRole: {skip: "always targets the config server"},
        revokeRolesFromUser: {skip: "always targets the config server"},
        rolesInfo: {skip: "always targets the config server"},
        rotateCertificates: {skip: "executes locally on mongos (not sent to any remote node)"},
        rotateFTDC: {skip: "executes locally on mongos (not sent to any remote node)"},
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
                command: function (dbName, collName) {
                    return {
                        setIndexCommitQuorum: collName,
                        indexNames: ["index"],
                        commitQuorum: "majority",
                    };
                },
            },
        },
        setFeatureCompatibilityVersion: {skip: "not on a user database"},
        setProfilingFilterGlobally: {skip: "executes locally on mongos (not sent to any remote node)"},
        setParameter: {skip: "executes locally on mongos (not sent to any remote node)"},
        setClusterParameter: {skip: "always targets the config server"},
        setQuerySettings: {skip: "not on a user database"},
        removeQuerySettings: {skip: "not on a user database"},
        setUserWriteBlockMode: {skip: "executes locally on mongos (not sent to any remote node)"},
        shardCollection: {skip: "does not forward command to primary shard"},
        shardDrainingStatus: {skip: "not on a user database"},
        shutdown: {skip: "does not forward command to primary shard"},
        split: {skip: "does not forward command to primary shard"},
        splitVector: {skip: "does not forward command to primary shard"},
        getTrafficRecordingStatus: {skip: "executes locally on targeted node"},
        startRecordingTraffic: {skip: "Renamed to startTrafficRecording"},
        stopRecordingTraffic: {skip: "Renamed to stopTrafficRecording"},
        startShardDraining: {skip: "not on a user database"},
        startTrafficRecording: {skip: "executes locally on mongos (not sent to any remote node)"},
        startSession: {skip: "executes locally on mongos (not sent to any remote node)"},
        stopShardDraining: {skip: "not on a user database"},
        stopTrafficRecording: {skip: "executes locally on mongos (not sent to any remote node)"},
        testDeprecation: {skip: "executes locally on mongos (not sent to any remote node)"},
        testDeprecationInVersion2: {skip: "executes locally on mongos (not sent to any remote node)"},
        testInternalTransactions: {skip: "executes locally on mongos (not sent to any remote node)"},
        testRemoval: {skip: "executes locally on mongos (not sent to any remote node)"},
        testVersion2: {skip: "executes locally on mongos (not sent to any remote node)"},
        testVersions1And2: {skip: "executes locally on mongos (not sent to any remote node)"},
        transitionFromDedicatedConfigServer: {skip: "not on a user database"},
        transitionToDedicatedConfigServer: {skip: "not on a user database"},
        unshardCollection: {skip: "does not forward command to primary shard"},
        untrackUnshardedCollection: {skip: "does not forward command to primary shard"},
        update: {
            run: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        update: collName,
                        updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}],
                    };
                },
            },
            explain: {
                sendsDbVersion: true,
                command: function (dbName, collName) {
                    return {
                        explain: {
                            update: collName,
                            updates: [{q: {_id: 2}, u: {_id: 2}, upsert: true, multi: false}],
                        },
                    };
                },
            },
        },
        updateRole: {skip: "always targets the config server"},
        updateSearchIndex: {skip: "executes locally on mongos", conditional: true},
        updateUser: {skip: "always targets the config server"},
        updateZoneKeyRange: {skip: "not on a user database"},
        usersInfo: {skip: "always targets the config server"},
        validate: {
            run: {
                sendsDbVersion: true,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {validate: collName};
                },
            },
        },
        validateDBMetadata: {
            run: {
                // validateDBMetadata is always broadcast to all shards.
                sendsDbVersion: false,
                explicitlyCreateCollection: true,
                command: function (dbName, collName) {
                    return {validateDBMetadata: 1, apiParameters: {version: "1"}};
                },
            },
        },
        waitForFailPoint: {skip: "executes locally on mongos (not sent to any remote node)"},
        whatsmyuri: {skip: "executes locally on mongos (not sent to any remote node)"},
    },
    mongod: {
        _addShard: {skip: "not on a user database"},
        _configsvrAbortReshardCollection: {skip: "TODO"},
        _configsvrAddShard: {skip: "not on a user database"},
        _configsvrAddShardToZone: {skip: "TODO"},
        _configsvrBalancerCollectionStatus: {skip: "TODO"},
        _configsvrBalancerStart: {skip: "TODO"},
        _configsvrBalancerStatus: {skip: "TODO"},
        _configsvrBalancerStop: {skip: "TODO"},
        _configsvrCheckClusterMetadataConsistency: {skip: "TODO"},
        _configsvrCheckMetadataConsistency: {skip: "runs on the configserver"},
        _configsvrCleanupReshardCollection: {skip: "TODO"},
        _configsvrClearJumboFlag: {skip: "TODO"},
        _configsvrCollMod: {skip: "TODO"},
        _configsvrCommitChunkMigration: {skip: "TODO"},
        _configsvrCommitChunkSplit: {skip: "TODO"},
        _configsvrCommitChunksMerge: {skip: "TODO"},
        _configsvrCommitMergeAllChunksOnShard: {skip: "TODO"},
        _configsvrCommitMovePrimary: {skip: "TODO"},
        _configsvrCommitRefineCollectionShardKey: {skip: "TODO"},
        _configsvrCommitReshardCollection: {skip: "TODO"},
        _configsvrCommitShardRemoval: {skip: "runs on the configserver"},
        _configsvrConfigureCollectionBalancing: {skip: "TODO"},
        _configsvrCreateDatabase: {skip: "TODO"},
        _configsvrEnsureChunkVersionIsGreaterThan: {skip: "TODO"},
        _configsvrGetHistoricalPlacement: {skip: "TODO"},
        _configsvrMoveRange: {skip: "TODO"},
        _configsvrRemoveChunks: {skip: "TODO"},
        _configsvrRemoveShard: {skip: "TODO"},
        _configsvrRemoveShardFromZone: {skip: "TODO"},
        _configsvrRemoveTags: {skip: "TODO"},
        _configsvrRepairShardedCollectionChunksHistory: {skip: "TODO"},
        _configsvrResetPlacementHistory: {skip: "TODO"},
        _configsvrReshardCollection: {skip: "TODO"},
        _configsvrRunRestore: {skip: "TODO"},
        _configsvrSetAllowMigrations: {skip: "TODO"},
        _configsvrSetClusterParameter: {skip: "TODO"},
        _configsvrSetUserWriteBlockMode: {skip: "TODO"},
        _configsvrShardDrainingStatus: {skip: "TODO"},
        _configsvrStartShardDraining: {skip: "TODO"},
        _configsvrStopShardDraining: {skip: "TODO"},
        _configsvrTransitionFromDedicatedConfigServer: {skip: "TODO"},
        _configsvrTransitionToDedicatedConfigServer: {skip: "TODO"},
        _configsvrUpdateZoneKeyRange: {skip: "TODO"},
        _dropConnectionsToMongot: {skip: "TODO"},
        _dropMirrorMaestroConnections: {skip: "TODO", conditional: true},
        _flushDatabaseCacheUpdates: {skip: "TODO"},
        _flushDatabaseCacheUpdatesWithWriteConcern: {skip: "TODO"},
        _flushReshardingStateChange: {skip: "TODO"},
        _flushRoutingTableCacheUpdates: {skip: "TODO"},
        _flushRoutingTableCacheUpdatesWithWriteConcern: {skip: "TODO"},
        _getNextSessionMods: {skip: "TODO"},
        _getUserCacheGeneration: {skip: "TODO"},
        _hashBSONElement: {skip: "TODO"},
        _isSelf: {skip: "TODO"},
        _killOperations: {skip: "TODO"},
        _mergeAuthzCollections: {skip: "TODO"},
        _migrateClone: {skip: "TODO"},
        _mirrorMaestroConnPoolStats: {skip: "TODO", conditional: true},
        _mongotConnPoolStats: {skip: "TODO"},
        _recvChunkAbort: {skip: "TODO"},
        _recvChunkCommit: {skip: "TODO"},
        _recvChunkReleaseCritSec: {skip: "TODO"},
        _recvChunkStart: {skip: "TODO"},
        _recvChunkStatus: {skip: "TODO"},
        _refreshQueryAnalyzerConfiguration: {skip: "TODO"},
        _shardsvrAbortReshardCollection: {skip: "TODO"},
        _shardsvrBeginMigrationBlockingOperation: {skip: "TODO"},
        _shardsvrChangePrimary: {skip: "TODO"},
        _shardsvrCheckCanConnectToConfigServer: {skip: "TODO"},
        _shardsvrCheckMetadataConsistency: {
            run: {
                runsAgainstAdminDb: false,
                command: function (dbName, collName, dbVersionBefore, dbVersionAfter) {
                    return {_shardsvrCheckMetadataConsistency: 1};
                },
                expectedFailureCode: ErrorCodes.IllegalOperation,
            },
        },
        _shardsvrCheckMetadataConsistencyParticipant: {skip: "TODO"},
        _shardsvrCleanupReshardCollection: {skip: "TODO"},
        _shardsvrCleanupStructuredEncryptionData: {skip: "TODO"},
        _shardsvrCloneAuthoritativeMetadata: {skip: "TODO"},
        _shardsvrCloneCatalogData: {skip: "TODO"},
        _shardsvrCollMod: {skip: "TODO"},
        _shardsvrCollModParticipant: {skip: "TODO"},
        _shardsvrCommitCreateDatabaseMetadata: {skip: "TODO"},
        _shardsvrCommitDropDatabaseMetadata: {skip: "TODO"},
        _shardsvrCommitReshardCollection: {skip: "TODO"},
        _shardsvrCompactStructuredEncryptionData: {skip: "TODO"},
        _shardsvrConvertToCapped: {skip: "TODO"},
        _shardsvrConvertToCappedParticipant: {skip: "TODO"},
        _shardsvrCoordinateMultiUpdate: {skip: "TODO"},
        _shardsvrCreateCollection: {skip: "TODO"},
        _shardsvrCreateCollectionParticipant: {skip: "TODO"},
        _shardsvrDrainOngoingDDLOperations: {skip: "TODO"},
        _shardsvrDropCollection: {skip: "TODO"},
        _shardsvrDropCollectionIfUUIDNotMatchingWithWriteConcern: {skip: "TODO"},
        _shardsvrDropCollectionParticipant: {skip: "TODO"},
        _shardsvrDropDatabase: {
            run: {
                runsAgainstAdminDb: false,
                command: function (dbName, collName, dbVersionBefore, dbVersionAfter) {
                    return {
                        _shardsvrDropDatabase: dbName,
                        writeConcern: {w: "majority"},
                    };
                },
                expectedFailureCode: ErrorCodes.IllegalOperation,
            },
        },
        _shardsvrDropDatabaseParticipant: {skip: "TODO"},
        _shardsvrDropIndexes: {skip: "TODO"},
        _shardsvrDropIndexesParticipant: {skip: "TODO"},
        _shardsvrEndMigrationBlockingOperation: {skip: "TODO"},
        _shardsvrFetchCollMetadata: {skip: "TODO"},
        _shardsvrGetStatsForBalancing: {skip: "TODO"},
        _shardsvrJoinDDLCoordinators: {skip: "TODO"},
        _shardsvrJoinMigrations: {skip: "TODO"},
        _shardsvrMergeChunks: {skip: "TODO"},
        _shardsvrMergeAllChunksOnShard: {skip: "TODO"},
        _shardsvrMovePrimary: {skip: "TODO"},
        _shardsvrMovePrimaryEnterCriticalSection: {skip: "TODO"},
        _shardsvrMovePrimaryExitCriticalSection: {skip: "TODO"},
        _shardsvrMoveRange: {skip: "TODO"},
        _shardsvrNotifyShardingEvent: {skip: "TODO"},
        _shardsvrParticipantBlock: {skip: "TODO"},
        _shardsvrRefineCollectionShardKey: {skip: "TODO"},
        _shardsvrRenameCollection: {skip: "TODO"},
        _shardsvrRenameCollectionParticipant: {skip: "TODO"},
        _shardsvrRenameCollectionParticipantUnblock: {skip: "TODO"},
        _shardsvrRenameIndexMetadata: {skip: "TODO"},
        _shardsvrReshardCollection: {skip: "TODO"},
        _shardsvrReshardRecipientClone: {skip: "TODO"},
        _shardsvrReshardingDonorFetchFinalCollectionStats: {skip: "TODO"},
        _shardsvrReshardingDonorStartChangeStreamsMonitor: {skip: "TODO"},
        _shardsvrReshardingOperationTime: {skip: "TODO"},
        _shardsvrResolveView: {
            run: [
                {
                    runsAgainstAdminDb: false,
                    command: function (dbName, collName, dbVersionBefore, dbVersionAfter) {
                        return {
                            _shardsvrResolveView: 1,
                            nss: `${dbName}.${collName}`,
                            databaseVersion: dbVersionAfter,
                        };
                    },
                },
                {
                    runsAgainstAdminDb: false,
                    command: function (dbName, collName, dbVersionBefore, dbVersionAfter) {
                        return {
                            _shardsvrResolveView: 1,
                            nss: `${dbName}.${collName}`,
                            databaseVersion: dbVersionBefore,
                        };
                    },
                    expectedFailureCode: ErrorCodes.StaleDbVersion,
                },
            ],
        },
        _shardsvrRunSearchIndexCommand: {skip: "TODO"},
        _shardsvrSetAllowMigrations: {skip: "TODO"},
        _shardsvrSetClusterParameter: {skip: "TODO"},
        _shardsvrSetUserWriteBlockMode: {skip: "TODO"},
        _shardsvrUntrackUnsplittableCollection: {skip: "TODO"},
        _shardsvrValidateShardKeyCandidate: {skip: "TODO"},
        _transferMods: {skip: "TODO"},
        abortTransaction: {skip: "TODO"},
        aggregate: {skip: "TODO"},
        analyze: {skip: "TODO"},
        analyzeShardKey: {skip: "TODO"},
        appendOplogNote: {skip: "TODO"},
        applyOps: {skip: "TODO"},
        authenticate: {skip: "TODO"},
        autoCompact: {skip: "TODO"},
        autoSplitVector: {skip: "TODO"},
        buildInfo: {skip: "TODO"},
        bulkWrite: {skip: "TODO"},
        checkShardingIndex: {skip: "TODO"},
        cleanupOrphaned: {skip: "TODO"},
        cleanupStructuredEncryptionData: {skip: "TODO"},
        clearLog: {skip: "TODO"},
        cloneCollectionAsCapped: {skip: "TODO"},
        clusterAbortTransaction: {skip: "TODO"},
        clusterAggregate: {skip: "TODO"},
        clusterBulkWrite: {skip: "TODO"},
        clusterCommitTransaction: {skip: "TODO"},
        clusterCount: {skip: "TODO"},
        clusterDelete: {skip: "TODO"},
        clusterFind: {skip: "TODO"},
        clusterGetMore: {skip: "TODO"},
        clusterInsert: {skip: "TODO"},
        clusterUpdate: {skip: "TODO"},
        collMod: {skip: "TODO"},
        collStats: {skip: "TODO"},
        commitTransaction: {skip: "TODO"},
        compact: {skip: "TODO"},
        compactStructuredEncryptionData: {skip: "TODO"},
        configureFailPoint: {skip: "TODO"},
        configureQueryAnalyzer: {skip: "TODO"},
        connPoolStats: {skip: "TODO"},
        connPoolSync: {skip: "TODO"},
        connectionStatus: {skip: "TODO"},
        convertToCapped: {skip: "TODO"},
        coordinateCommitTransaction: {skip: "TODO"},
        count: {skip: "TODO"},
        cpuload: {skip: "TODO"},
        create: {skip: "TODO"},
        createIndexes: {skip: "TODO"},
        createRole: {skip: "TODO"},
        createSearchIndexes: {skip: "TODO"},
        createUser: {skip: "TODO"},
        currentOp: {skip: "TODO"},
        dataSize: {skip: "TODO"},
        dbCheck: {skip: "TODO"},
        dbHash: {skip: "TODO"},
        dbStats: {skip: "TODO"},
        delete: {skip: "TODO"},
        distinct: {skip: "TODO"},
        drop: {skip: "TODO"},
        dropAllRolesFromDatabase: {skip: "TODO"},
        dropAllUsersFromDatabase: {skip: "TODO"},
        dropConnections: {skip: "TODO"},
        dropDatabase: {skip: "TODO"},
        dropIndexes: {skip: "TODO"},
        dropRole: {skip: "TODO"},
        dropSearchIndex: {skip: "TODO"},
        dropUser: {skip: "TODO"},
        echo: {skip: "TODO"},
        endSessions: {skip: "TODO"},
        explain: {skip: "TODO"},
        exportCollection: {skip: "TODO", conditional: true},
        features: {skip: "TODO"},
        filemd5: {skip: "TODO"},
        find: {skip: "TODO"},
        findAndModify: {skip: "TODO"},
        flushRouterConfig: {skip: "TODO"},
        fsync: {skip: "TODO"},
        fsyncUnlock: {skip: "TODO"},
        getAuditConfig: {skip: "TODO", conditional: true},
        getChangeStreamState: {skip: "TODO"},
        getClusterParameter: {skip: "TODO"},
        getCmdLineOpts: {skip: "TODO"},
        getDatabaseVersion: {skip: "TODO"},
        getDefaultRWConcern: {skip: "TODO"},
        getDiagnosticData: {skip: "TODO"},
        getLog: {skip: "TODO"},
        getMore: {skip: "TODO"},
        getParameter: {skip: "TODO"},
        getQueryableEncryptionCountInfo: {skip: "TODO"},
        getShardMap: {skip: "TODO"},
        getShardVersion: {skip: "TODO"},
        getShardingReady: {skip: "TODO"},
        getTrafficRecordingStatus: {skip: "TODO"},
        godinsert: {skip: "TODO"},
        grantPrivilegesToRole: {skip: "TODO"},
        grantRolesToRole: {skip: "TODO"},
        grantRolesToUser: {skip: "TODO"},
        hello: {skip: "TODO"},
        hostInfo: {skip: "TODO"},
        httpClientRequest: {skip: "TODO"},
        importCollection: {skip: "TODO", conditional: true},
        insert: {skip: "TODO"},
        internalRenameIfOptionsAndIndexesMatch: {skip: "TODO"},
        invalidateUserCache: {skip: "TODO"},
        isMaster: {skip: "TODO"},
        killAllSessions: {skip: "TODO"},
        killAllSessionsByPattern: {skip: "TODO"},
        killCursors: {skip: "TODO"},
        killOp: {skip: "TODO"},
        killSessions: {skip: "TODO"},
        listCollections: {skip: "TODO"},
        listCommands: {skip: "TODO"},
        listDatabases: {skip: "TODO"},
        listDatabasesForAllTenants: {skip: "TODO"},
        listIndexes: {skip: "TODO"},
        listSearchIndexes: {skip: "TODO"},
        lockInfo: {skip: "TODO"},
        logApplicationMessage: {skip: "TODO", conditional: true},
        logMessage: {skip: "TODO"},
        logRotate: {skip: "TODO"},
        logout: {skip: "TODO"},
        makeSnapshot: {skip: "TODO"},
        mapReduce: {skip: "TODO"},
        oidcListKeys: {skip: "TODO", conditional: true},
        oidcRefreshKeys: {skip: "TODO", conditional: true},
        pinHistoryReplicated: {skip: "TODO"},
        ping: {skip: "TODO"},
        planCacheClear: {skip: "TODO"},
        planCacheClearFilters: {skip: "TODO"},
        planCacheListFilters: {skip: "TODO"},
        planCacheSetFilter: {skip: "TODO"},
        prepareTransaction: {skip: "TODO"},
        profile: {skip: "TODO"},
        reIndex: {skip: "TODO"},
        reapLogicalSessionCacheNow: {skip: "TODO"},
        refreshLogicalSessionCacheNow: {skip: "TODO"},
        refreshSessions: {skip: "TODO"},
        releaseMemory: {skip: "TODO"},
        removeQuerySettings: {skip: "TODO"},
        renameCollection: {skip: "TODO"},
        replSetAbortPrimaryCatchUp: {skip: "TODO"},
        replSetFreeze: {skip: "TODO"},
        replSetGetConfig: {skip: "TODO"},
        replSetGetRBID: {skip: "TODO"},
        replSetGetStatus: {skip: "TODO"},
        replSetHeartbeat: {skip: "TODO"},
        replSetInitiate: {skip: "TODO"},
        replSetMaintenance: {skip: "TODO"},
        replSetReconfig: {skip: "TODO"},
        replSetRequestVotes: {skip: "TODO"},
        replSetResizeOplog: {skip: "TODO"},
        replSetStepDown: {skip: "TODO"},
        replSetStepUp: {skip: "TODO"},
        replSetSyncFrom: {skip: "TODO"},
        replSetTest: {skip: "TODO"},
        replSetTestEgress: {skip: "TODO"},
        replSetUpdatePosition: {skip: "TODO"},
        revokePrivilegesFromRole: {skip: "TODO"},
        revokeRolesFromRole: {skip: "TODO"},
        revokeRolesFromUser: {skip: "TODO"},
        rolesInfo: {skip: "TODO"},
        rotateCertificates: {skip: "TODO"},
        rotateFTDC: {skip: "TODO"},
        saslContinue: {skip: "TODO"},
        saslStart: {skip: "TODO"},
        serverStatus: {skip: "TODO"},
        setAuditConfig: {skip: "TODO", conditional: true},
        setChangeStreamState: {skip: "TODO"},
        setClusterParameter: {skip: "TODO"},
        setCommittedSnapshot: {skip: "TODO"},
        setDefaultRWConcern: {skip: "TODO"},
        setFeatureCompatibilityVersion: {skip: "TODO"},
        setIndexCommitQuorum: {skip: "TODO"},
        setParameter: {skip: "TODO"},
        setProfilingFilterGlobally: {skip: "TODO"},
        setQuerySettings: {skip: "TODO"},
        setUserWriteBlockMode: {skip: "TODO"},
        shardingState: {skip: "TODO"},
        shutdown: {skip: "TODO"},
        sleep: {skip: "TODO"},
        splitChunk: {skip: "TODO"},
        splitVector: {skip: "TODO"},
        startSession: {skip: "TODO"},
        startTrafficRecording: {skip: "TODO"},
        stopTrafficRecording: {skip: "TODO"},
        streams_getMetrics: {skip: "TODO", conditional: true},
        streams_getMoreStreamSample: {skip: "TODO", conditional: true},
        streams_getStats: {skip: "TODO", conditional: true},
        streams_listStreamProcessors: {skip: "TODO", conditional: true},
        streams_sendEvent: {skip: "TODO", conditional: true},
        streams_startStreamProcessor: {skip: "TODO", conditional: true},
        streams_startStreamSample: {skip: "TODO", conditional: true},
        streams_stopStreamProcessor: {skip: "TODO", conditional: true},
        streams_testOnlyGetFeatureFlags: {skip: "TODO", conditional: true},
        streams_testOnlyInsert: {skip: "TODO", conditional: true},
        streams_updateConnection: {skip: "TODO", conditional: true},
        streams_updateFeatureFlags: {skip: "TODO", conditional: true},
        streams_writeCheckpoint: {skip: "TODO", conditional: true},
        sysprofile: {skip: "TODO"},
        testCommandFeatureFlaggedOnLatestFCV83: {skip: "internal command", conditional: true},
        testDeprecation: {skip: "TODO", conditional: true},
        testDeprecationInVersion2: {skip: "TODO", conditional: true},
        testInternalTransactions: {skip: "TODO", conditional: true},
        testRemoval: {skip: "TODO", conditional: true},
        testReshardCloneCollection: {skip: "TODO", conditional: true},
        testVersion2: {skip: "TODO", conditional: true},
        testVersions1And2: {skip: "TODO", conditional: true},
        timeseriesCatalogBucketParamsChanged: {skip: "TODO", conditional: true},
        top: {skip: "TODO"},
        transitionToShardedCluster: {skip: "TODO"},
        update: {skip: "TODO"},
        updateRole: {skip: "TODO"},
        updateSearchIndex: {skip: "TODO"},
        updateUser: {skip: "TODO"},
        usersInfo: {skip: "TODO"},
        validate: {skip: "TODO"},
        validateDBMetadata: {skip: "TODO"},
        voteAbortIndexBuild: {skip: "TODO"},
        voteCommitImportCollection: {skip: "TODO", conditional: true},
        voteCommitIndexBuild: {skip: "TODO"},
        waitForFailPoint: {skip: "TODO", conditional: true},
        whatsmysni: {skip: "TODO"},
        whatsmyuri: {skip: "TODO"},
    },
};

// TODO (SERVER-101777): This test makes a lot of assumptions about database versions stored in
// shards that are not the primary shard. Hence we turn off all shardAuthoritative feature flags.
const shardOptions = (() => {
    if (Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion)) {
        return {};
    }
    return {
        setParameter: {
            featureFlagShardAuthoritativeDbMetadataDDL: false,
            featureFlagShardAuthoritativeDbMetadataCRUD: false,
            featureFlagShardAuthoritativeCollMetadata: false,
        },
    };
})();
const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    other: {
        configOptions: shardOptions,
        rsOptions: shardOptions,
    },
});

const doTest = (connection, testCases, commandsAddedSinceLastLTS, commandsRemovedSinceLastLTS) => {
    const listCommandsRes = connection.adminCommand({listCommands: 1});
    assert.commandWorked(listCommandsRes);
    print("--------------------------------------------");
    for (const command of Object.keys(listCommandsRes.commands)) {
        print(command);
    }

    const isMultiversion = Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet);

    commandsRemovedSinceLastLTS.forEach(function (cmd) {
        testCases[cmd] = {
            skip: "must define test coverage for latest version backwards compatibility",
        };
    });

    (() => {
        // Validate test cases for all commands.

        // Ensure there is a test case for every mongos command, and that the test cases are
        // well formed.
        for (const command of Object.keys(listCommandsRes.commands)) {
            const testCase = testCases[command];
            assert(testCase !== undefined, "coverage failure: must define a test case for " + command);
            validateTestCase(testCase, connection == st.s);
            testCases[command].validated = true;
        }

        // After iterating through all the existing commands, ensure there were no additional
        // test cases that did not correspond to any mongos command.
        for (const key of Object.keys(testCases)) {
            // We have defined real test cases for commands added since the last LTS version so
            // that the test cases are exercised in the regular suites, but because these test
            // cases can't run in the last stable suite, we skip processing them here to avoid
            // failing the below assertion. We have defined "skip" test cases for commands
            // removed since the last LTS version so the test case is defined in last stable
            // suites (in which these commands still exist on the mongos), but these test cases
            // won't be run in regular suites, so we skip processing them below as well.
            if (commandsAddedSinceLastLTS.includes(key) || commandsRemovedSinceLastLTS.includes(key)) {
                continue;
            }
            assert(
                testCases[key].validated || testCases[key].conditional,
                `you defined a test case for a command '${key}' that does not exist: ${tojson(testCases[key])}`,
            );
        }
    })();

    (() => {
        // Test that commands that send databaseVersion are subjected to the databaseVersion
        // check when the primary shard for the database has moved and the database no longer
        // exists on the old primary shard (because the database only contained unsharded
        // collections; this is in anticipation of SERVER-43925).

        const dbName = getNewDbName();
        const collName = "foo";

        // Create the database by creating a collection in it.
        assert.commandWorked(st.s0.getDB(dbName).createCollection("dummy"));

        for (const command of Object.keys(listCommandsRes.commands)) {
            const testCase = testCases[command];
            if (testCase.skip || (isMultiversion && testCase.skipMultiversion)) {
                print("skipping " + command + ": " + testCase.skip);
                continue;
            }

            for (const test of toArray(testCase.run)) {
                testCommandAfterMovePrimary(test, connection, st, dbName, collName);
            }
            if (testCase.explain) {
                for (const test of toArray(testCase.explain)) {
                    testCommandAfterMovePrimary(test, connection, st, dbName, collName);
                }
            }
        }
    })();

    (() => {
        // Test that commands that send databaseVersion are subjected to the databaseVersion
        // check when the primary shard for the database has moved, but the database still
        // exists on the old primary shard (because the old primary shard owns chunks for
        // sharded collections in the database).

        const dbName = getNewDbName();
        const collName = "foo";
        const shardedCollName = "pinnedShardedCollWithChunksOnBothShards";

        // Create a sharded collection with data on both shards so that the database does not
        // get dropped on the old primary shard after movePrimary.
        const shardedCollNs = dbName + "." + shardedCollName;
        assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
        assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard0.shardName, zone: "x < 0"}));
        assert.commandWorked(st.s0.adminCommand({addShardToZone: st.shard1.shardName, zone: "x >= 0"}));
        assert.commandWorked(
            st.s0.adminCommand({updateZoneKeyRange: shardedCollNs, min: {x: MinKey}, max: {x: 0}, zone: "x < 0"}),
        );
        assert.commandWorked(
            st.s0.adminCommand({updateZoneKeyRange: shardedCollNs, min: {x: 0}, max: {x: MaxKey}, zone: "x >= 0"}),
        );
        assert.commandWorked(st.s0.getDB("admin").admin.runCommand({shardCollection: shardedCollNs, key: {"x": 1}}));
        assert(containsCollection(st.shard0, dbName, shardedCollName));
        assert(containsCollection(st.shard1, dbName, shardedCollName));

        for (const command of Object.keys(listCommandsRes.commands)) {
            const testCase = testCases[command];
            if (testCase.skip || (isMultiversion && testCase.skipMultiversion)) {
                print("skipping " + command + ": " + testCase.skip);
                continue;
            }

            for (const test of toArray(testCase.run)) {
                testCommandAfterMovePrimary(test, connection, st, dbName, collName);
            }
            if (testCase.explain) {
                for (const test of toArray(testCase.explain)) {
                    testCommandAfterMovePrimary(test, connection, st, dbName, collName);
                }
            }
        }
    })();

    (() => {
        // Test that commands that send databaseVersion are subjected to the databaseVersion
        // check when the database has been dropped and recreated with a different primary
        // shard.

        for (const command of Object.keys(listCommandsRes.commands)) {
            const testCase = testCases[command];
            if (testCase.skip || (isMultiversion && testCase.skipMultiversion)) {
                print("skipping " + command + ": " + testCase.skip);
                continue;
            }

            for (const test of toArray(testCase.run)) {
                testCommandAfterDropRecreateDatabase(test, connection, st);
            }
            if (testCase.explain) {
                for (const test of toArray(testCase.explain)) {
                    testCommandAfterDropRecreateDatabase(test, connection, st);
                }
            }
        }
    })();
};

doTest(st.s, allTestCases.mongos, commandsAddedToMongosSinceLastLTS, commandsRemovedFromMongosSinceLastLTS);
doTest(
    st.rs0.getPrimary(),
    allTestCases.mongod,
    commandsAddedToMongodSinceLastLTS,
    commandsRemovedFromMongodSinceLastLTS,
);

st.stop();
