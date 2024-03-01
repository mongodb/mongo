/**
 * This test ensures that commands run on an unsharded collection in a sharded cluster have the same
 * behaviour as when run on a replica set. Specifically, it ensures the response of the sharded
 * cluster is compatible with the response of the replica set, by checking that the former is a
 * superset of the latter.
 *
 * Tests are declared in the "tests" array, and each test may have multiple 'testCases'. Each test
 * has the following fields:
 *
 * 1) setup
 *
 * The setup function is called before executing each testCase.
 *
 * 2) teardown
 *
 * The teardown function is called immediately after testing each testCase.
 *
 * 3) database
 *
 * The name of the database where the command is going to be executed.
 *
 * 4) supportsWriteConcern
 *
 * Boolean field, used to skip writeConcern tests for commands that do not support writeConcern.
 *
 * 4) testCases
 *
 * Array defining each of the cases to be tested for each command.
 *
 *     testCase fields:
 *
 *     4.1) shortDescription
 *
 *     Offers a short description of the case that is being tested.
 *
 *     4.2) skipSetup
 *
 *     Boolean field, used to skip setup for the testCase.
 *
 *     4.3) command
 *
 *     The command that is going to be executed.
 *
 *     4.4) expectedError
 *
 *     Field indicating the error code that is expected to be returned by the command.
 *     The testCases that are expected to fail are responses from the replica
 *     set or the shard and not parser errors.
 *
 *     4.5) testCaseSetup
 *
 *     The testCaseSetup function is called before executing each testCase.
 *
 *     4.6) testCaseDoesNotSupportWriteConcern
 *
 *     Boolean field, used to skip a writeConcern testCase that belongs to a test that accepts
 *     writeConcern.
 *
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertWriteConcernError,
    restartReplicationOnAllShards,
    restartReplicationOnSecondaries,
    stopReplicationOnSecondaries,
    stopReplicationOnSecondariesOfAllShards
} from "jstests/libs/write_concern_util.js";

const tests = [

    {
        name: "validate",
        database: "test",
        setup: function(db) {
            assert.commandWorked(db.x.insert({}));
        },
        teardown: function(db) {
            db.x.drop();
        },
        supportsWriteConcern: false,
        testCases: [
            {
                shortDescription: "Runs validate expecting an error.",
                command: {validate: "x"},
                skipSetup: true,
                expectedError: ErrorCodes.NamespaceNotFound,
            },
            {
                shortDescription: "Runs validate on an empty collection.",
                testCaseSetup: function(db) {
                    assert.commandWorked(db.createCollection("x"));
                },
                command: {validate: "x"},
                skipSetup: true,
            },
            {
                shortDescription: "Runs validate without optional fields.",
                command: {validate: "x"},
            },
            {
                shortDescription: "Runs validate with full: true.",
                command: {validate: "x", full: true},
            },
        ]
    },
    {
        name: "collMod",
        database: "test",
        setup: function(db) {
            assert.commandWorked(db.x.createIndex({type: 1}));
        },
        teardown: function(db) {
            db.x.drop();
        },
        // writeConcernError is retriable in mongos collMod command.
        supportsWriteConcern: false,
        testCases: [
            {
                shortDescription: "Runs collMod expecting an error.",
                command: {
                    collMod: "x",
                    index: {keyPattern: {type: 1}, hidden: true},
                },
                skipSetup: true,
                expectedError: ErrorCodes.NamespaceNotFound,
            },
            {
                shortDescription: "collMod to change expireAfterSeconds.",
                command: {collMod: "x", index: {keyPattern: {type: 1}, expireAfterSeconds: 3600}},
            },
            {
                shortDescription: "Hide an Index from the Query Planner.",
                command: {
                    collMod: "x",
                    index: {keyPattern: {type: 1}, hidden: true},
                },
            },
            {
                shortDescription: "Set a new expiration for entries with collMod.",
                command: {
                    collMod: "x",
                    index: {keyPattern: {type: 1}, expireAfterSeconds: 5},
                },
            },
        ]
    },
    {
        name: "dbStats",
        database: "test",
        supportsWriteConcern: false,
        testCases: [
            {
                shortDescription: "Runs dbStats with freeStorage=0.",
                command: {dbStats: 1, scale: 1024, freeStorage: 0},
            },
            {
                shortDescription: "Runs dbStats with freeStorage=1.",
                command: {dbStats: 1, scale: 1024, freeStorage: 1},
            },
        ]
    },
    {
        name: "appendOplogNote",
        database: "admin",
        supportsWriteConcern: true,
        testCases: [
            {
                shortDescription: "Runs appendOplogNote.",
                command: {appendOplogNote: 1, data: {msg: "message"}},
            },
        ]
    },
    {
        name: "setIndexCommitQuorum",
        database: "test",
        supportsWriteConcern: true,
        setup: function(db) {
            // Insert a document to avoid empty collection optimisation for index build.
            assert.commandWorked(db.x.insert({}));
            const primary = FixtureHelpers.getPrimaryForNodeHostingDatabase(db);
            let awaitShell;
            const failPoint = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
            awaitShell = startParallelShell(function() {
                assert.commandWorked(db.runCommand({
                    createIndexes: "x",
                    indexes: [{key: {a: 1}, name: "a_1"}],
                }));
            }, db.getMongo().port);
            failPoint.wait();
            return {failPoint: failPoint, awaitShell: awaitShell};
        },
        teardown: function(db, ctx) {
            ctx.failPoint.off();
            ctx.awaitShell();
            db.x.drop();
        },
        testCases: [
            {
                shortDescription: "Runs setIndexCommitQuorum expecting an error.",
                command: {setIndexCommitQuorum: "x", indexNames: ["err"], commitQuorum: 1},
                expectedError: ErrorCodes.IndexNotFound,
            },
            {
                shortDescription: "Runs setIndexCommitQuorum without a message.",
                command: {setIndexCommitQuorum: "x", indexNames: ["a_1"], commitQuorum: 1},
            },
            {
                shortDescription: "Runs setIndexCommitQuorum with a message.",
                command: {
                    setIndexCommitQuorum: "x",
                    indexNames: ["a_1"],
                    commitQuorum: 1,
                    comment: "message"
                },
            },
        ]
    },
    {
        name: "createIndexes",
        database: "test",
        supportsWriteConcern: true,
        teardown: function(db) {
            db.x.drop();
        },
        testCases: [
            {
                shortDescription: "Runs createIndexes expecting an index option conflict error.",
                testCaseSetup: function(db) {
                    assert.commandWorked(
                        db.runCommand({createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]}));
                },
                command: {createIndexes: "x", indexes: [{key: {a: 1}, name: "err"}]},
                expectedError: ErrorCodes.IndexOptionsConflict,
            },
            {
                shortDescription:
                    "Runs createIndexes expecting 'createdCollectionAutomatically' : true.",
                command: {createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]},
                testCaseDoesNotSupportWriteConcern: true,
            },

            {
                shortDescription:
                    "Runs createIndexes expecting 'note' : 'all indexes already exist'",
                testCaseSetup: function(db) {
                    assert.commandWorked(
                        db.runCommand({createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]}));
                },
                command: {createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]},
                testCaseDoesNotSupportWriteConcern: true,
            },
            {
                shortDescription: "Runs createIndexes on an existing empty collection.",
                testCaseSetup: function(db) {
                    assert.commandWorked(db.createCollection("x"));
                },
                command: {createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]},
            },
            {
                shortDescription:
                    "Runs createIndexes on an existing collection with data (causing an index build).",
                testCaseSetup: function(db) {
                    assert.commandWorked(db.x.insert({}));
                },
                command: {createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]},
                testCaseDoesNotSupportWriteConcern: true,
            },
        ]
    },
    {
        name: "dropIndexes",
        database: "test",
        supportsWriteConcern: true,
        teardown: function(db) {
            db.x.drop();
        },
        testCases: [
            {
                shortDescription: "Runs dropIndexes expecting an error.",
                command: {dropIndexes: "x", index: ["a_1"]},
                expectedError: ErrorCodes.NamespaceNotFound,
            },
            {
                shortDescription: "Runs dropIndexes.",
                testCaseSetup: function(db) {
                    assert.commandWorked(
                        db.runCommand({createIndexes: "x", indexes: [{key: {a: 1}, name: "a_1"}]}));
                },
                command: {dropIndexes: "x", index: ["a_1"]},
            }
        ]
    },
    {
        name: "cpuload",
        database: "test",
        testCases: [
            {
                shortDescription: "Runs cpuload.",
                command: {cpuload: 1, cpuFactor: 1},
            },
        ]
    },
    {
        name: "lockInfo",
        database: "admin",
        testCases: [
            {
                shortDescription: "Runs lockInfo.",
                command: {lockInfo: 1},
            },
        ]
    },
];

// Test parity between the collMod API of unsharded, unsplitable collections and
// sharded collections
function collModParity(db, collModCommand) {
    // Run collmod on a unsplitable collection
    db.runCommand({createUnsplittableCollection: "x"});
    db.runCommand({createIndexes: "x", indexes: [{key: {"age": 1}, name: "ageIndex"}]});
    collModCommand.command.collMod = "x"
    const unsplitableResultKeys = Object.keys(runAndAssertTestCase(collModCommand, db));

    // Run collmod on an unsharded collection
    db.runCommand({create: "y"});
    db.runCommand({createIndexes: "y", indexes: [{key: {"age": 1}, name: "ageIndex"}]});
    collModCommand.command.collMod = "y"
    const unshardedResultKeys = Object.keys(runAndAssertTestCase(collModCommand, db));

    // Run collmod on an sharded collection
    db.runCommand({create: "z"});
    db.runCommand({createIndexes: "z", indexes: [{key: {"age": 1}, name: "ageIndex"}]});
    collModCommand.command.collMod = "z"
    const shardedResultKeys = Object.keys(runAndAssertTestCase(collModCommand, db));

    assert(isSubset(unsplitableResultKeys, unshardedResultKeys) &&
               isSubset(unshardedResultKeys, shardedResultKeys) &&
               isSubset(shardedResultKeys, unsplitableResultKeys),
           "Missing fields in the response" +
               "\nKeys in unsplitable (sharded) collection response: " + unsplitableResultKeys +
               "\nKeys in unshardedResultKeys collection response: " + unshardedResultKeys +
               "\nKeys in sharded collection response: " + shardedResultKeys);

    db.x.drop();
    db.y.drop();
    db.z.drop();
}

function collModParityTests(db) {
    collModParity(db, {
        command: {
            collMod: "",  // Filled by collModParity
            index: {name: "ageIndex", hidden: true}
        }
    });
    collModParity(db, {
        command: {
            collMod: "",  // Filled by collModParity
            index: {name: "ageIndex", expireAfterSeconds: NumberLong(5)}
        }
    });
    collModParity(db, {
        command: {
            collMod: "",  // Filled by collModParity
            index: {name: "ageIndex", expireAfterSeconds: 5}
        }
    });
    collModParity(db, {
        command: {
            collMod: "",  // Filled by collModParity
            index: {name: "ageIndex", prepareUnique: true}
        }
    });
}

function runAndAssertTestCaseWithForcedWriteConcern(testCase, testFixture) {
    testFixture.stopReplication(testFixture.mongoConfig)
    testCase.command.writeConcern = {w: "majority", wtimeout: 1};
    const result = testFixture.db.runCommand(testCase.command);
    assertWriteConcernError(result);
    assert.commandWorkedIgnoringWriteConcernErrors(result);
    testFixture.restartReplication(testFixture.mongoConfig);
    return result;
}

function runAndAssertTestCase(testCase, db) {
    const result = db.runCommand(testCase.command);
    if (testCase.expectedError) {
        assert.commandFailedWithCode(result, testCase.expectedError);
    } else {
        assert.commandWorked(result);
    }
    return result;
}

/**
 * runTestCase function runs a testCase in a given database
 * and returns the result.
 */
function runTestCase(test, testCase, forceWriteConcernError, testFixture) {
    var ctx;
    const db = testFixture.db;
    if (!testCase.skipSetup && test.setup) {
        ctx = test.setup(db);
    }
    if (testCase.testCaseSetup) {
        testCase.testCaseSetup(db);
    }

    let result;
    if (forceWriteConcernError) {
        result = runAndAssertTestCaseWithForcedWriteConcern(testCase, testFixture);
    } else {
        result = runAndAssertTestCase(testCase, db);
    }

    if (test.teardown) {
        test.teardown(db, ctx);
    }
    return result;
}

/**
 * Checks if an array is a subset of another array.
 */
function isSubset(superset, subset) {
    return subset.every(el => superset.includes(el));
}

function assertMongosAndReplicaSetInterfaceParity(test, testCase, forceWriteConcernError, st, rst) {
    const mongosDb = st.getDB(test.database);
    const replicaSetDb = rst.getPrimary().getDB(test.database);

    const replicaSetTestFixture = {
        db: replicaSetDb,
        mongoConfig: rst,
        stopReplication: stopReplicationOnSecondaries,
        restartReplication: restartReplicationOnSecondaries,
    }

    const mongosTestFixture = {
        db: mongosDb,
        mongoConfig: st,
        stopReplication: stopReplicationOnSecondariesOfAllShards,
        restartReplication: restartReplicationOnAllShards,
    }

    const replSetResultKeys = Object.keys(runTestCase(
        test,
        testCase,
        forceWriteConcernError,
        replicaSetTestFixture,
        ));

    const shardResultKeys = Object.keys(runTestCase(
        test,
        testCase,
        forceWriteConcernError,
        mongosTestFixture,
        ));

    assert(isSubset(shardResultKeys, replSetResultKeys),
           "Missing fields in the mongos response" +
               "\nKeys in replica set response: " + replSetResultKeys +
               "\nKeys in shard response: " + shardResultKeys + "\nTest case: " + tojson(testCase));
}

function runTestCases(st, rst, forceWriteConcernError) {
    tests.forEach((test) => {
        if (forceWriteConcernError && !test.supportsWriteConcern) {
            return;
        }

        test.testCases.forEach((testCase) => {
            if (forceWriteConcernError &&
                (testCase.expectedError || testCase.testCaseDoesNotSupportWriteConcern)) {
                return;
            }
            assertMongosAndReplicaSetInterfaceParity(
                test, testCase, forceWriteConcernError, st, rst);
        });
    });
}

const st = new ShardingTest({shards: {rs0: {nodes: [{}, {rsConfig: {priority: 0}}]}}});
const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});

rst.startSet();
rst.initiate();

runTestCases(st, rst, false);
runTestCases(st, rst, true);

collModParityTests(st.s.getDB("test"));
collModParityTests(rst.getPrimary().getDB("test"));

st.stop();
rst.stopSet();
