/*
This test ensures that commands run on an unsharded collection in a sharded cluster have the same
behaviour as when run on a replica set. Specifically, it ensures the response of the sharded cluster
is compatible with the response of the replica set, by checking that the former is a superset of the
latter.

Tests are declared in the "tests" array, and each test may have multiple 'testcases'. Each test has
the following fields:

1) setup

The setup function is called before executing each testcase.

2) teardown

The teardown function is called immediately after testing each testcase.

3) database

The name of the database where the command is going to be executed.

4) testscases

Array defining each of the cases to be tested for each command.

    testcase fields:

    4.1) shortDescription

    Offers a short description of the case that is being tested.

    4.2) skipSetup

    Boolean field, used to skip setup for the testcase.

    4.3) command

    The command that is going to be executed.

    4.4) expectFail

    Boolean field indicating if the command is expected to fail and return error.
*/

(function() {
load("jstests/libs/fail_point_util.js");
load("jstests/libs/fixture_helpers.js");

const tests = [
    {
        name: "collMod",
        database: "test",
        setup: function(db) {
            assert.commandWorked(db.x.createIndex({type: 1}));
        },
        teardown: function(db) {
            db.x.drop();
        },
        testcases: [
            {
                shortDescription: "Runs collMod expecting an error.",
                command: {
                    collMod: "x",
                    index: {keyPattern: {type: 1}, hidden: true},
                },
                skipSetup: true,
                expectFail: true,
            },
            {
                shortDescription: "collMod to change expireAfterSeconds.",
                command: {collMod: "x", index: {keyPattern: {type: 1}, expireAfterSeconds: 3600}},
                expectFail: false,
            },
            {
                shortDescription: "Hide an Index from the Query Planner.",
                command: {
                    collMod: "x",
                    index: {keyPattern: {type: 1}, hidden: true},
                },
                expectFail: false,
            },
        ]
    },
    {
        name: "dbStats",
        database: "test",
        testcases: [
            {
                shortDescription: "Runs dbStats expecting an error.",
                command: {dbStats: 1, scale: -1, freeStorage: 0},
                expectFail: true,
            },
            {
                shortDescription: "Runs dbStats with freeStorage=0.",
                command: {dbStats: 1, scale: 1024, freeStorage: 0},
                expectFail: false,
            },
            {
                shortDescription: "Runs dbStats with freeStorage=1.",
                command: {dbStats: 1, scale: 1024, freeStorage: 1},
                expectFail: false,
            },
        ]
    },
    {
        name: "appendOplogNote",
        database: "admin",
        testcases: [
            {
                shortDescription: "Runs appendOplogNote expecting an error.",
                command: {appendOplogNote: 1},
                expectFail: true,
            },
            {
                shortDescription: "Runs appendOplogNote.",
                command: {appendOplogNote: 1, data: {msg: "message"}},
                expectFail: false,
            },
        ]
    },
    {
        name: "setIndexCommitQuorum",
        database: "test",
        setup: function(db) {
            // Insert a document to avoid empty collection optimisation for index build.
            assert.commandWorked(db.x.insert({}));
            primary = FixtureHelpers.getPrimaryForNodeHostingDatabase(db);
            let awaitShell;
            const failPoint = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
            awaitShell = startParallelShell(function() {
                assert.commandWorked(db.runCommand({
                    createIndexes: "x",
                    indexes: [{key: {a: 1}, name: "a_1"}],
                    commitQuorum: 1,
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
        testcases: [
            {
                shortDescription: "Runs setIndexCommitQuorum expecting an error.",
                command: {setIndexCommitQuorum: "x", indexNames: ["err"], commitQuorum: 1},
                expectFail: true,
            },
            {
                shortDescription: "Runs setIndexCommitQuorum without a message.",
                command: {setIndexCommitQuorum: "x", indexNames: ["a_1"], commitQuorum: 1},
                expectFail: false,
            },
            {
                shortDescription: "Runs setIndexCommitQuorum with a message.",
                command: {
                    setIndexCommitQuorum: "x",
                    indexNames: ["a_1"],
                    commitQuorum: 1,
                    comment: "message"
                },
                expectFail: false,
            },
        ]
    },
];

/* runTestcase function runs a testcase in a given database
and returns the result */
function runTestcase(db, test, testcase) {
    var ctx;
    if (!testcase.skipSetup) {
        if (test.setup) {
            ctx = test.setup(db);
        }
    }
    const result = db.runCommand(testcase.command);
    if (!testcase.expectFail) {
        assert.commandWorked(result);
    }
    if (test.teardown) {
        test.teardown(db, ctx);
    }
    return result;
}

// Checks if an array is a subset of another array
function isSubset(superset, subset) {
    return subset.every(el => superset.includes(el));
}

const st = new ShardingTest({shards: [{nodes: 1}]});
const rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiate();

tests.forEach((test) => {
    const mongosDb = st.getDB(test.database);
    const replicaSetDb = rst.getPrimary().getDB(test.database);

    test.testcases.forEach((testcase) => {
        const replSetResultKeys = Object.keys(runTestcase(replicaSetDb, test, testcase));
        const shardResultKeys = Object.keys(runTestcase(mongosDb, test, testcase));

        assert(isSubset(shardResultKeys, replSetResultKeys),
               "Missing fields in the mongos response" +
                   "\nKeys in replica set response: " + replSetResultKeys +
                   "\nKeys in shard response: " + shardResultKeys +
                   "\nTestcase Object: " + tojson(testcase));
    });
});

st.stop();
rst.stopSet();
}());
