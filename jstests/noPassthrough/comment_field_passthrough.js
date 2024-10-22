/**
 * Verify that adding 'comment' field to any command shouldn't cause unexpected failures.
 * @tags: [
 *   requires_capped,
 *   requires_persistence,
 *   requires_replication,
 *   requires_sharding,
 *   requires_scripting,
 * ]
 */

import {authCommandsLib} from "jstests/auth/lib/commands_lib.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const tests = authCommandsLib.tests;

// The following commands require additional start up configuration and hence need to be skipped.
const denylistedTests = [
    "startRecordingTraffic",
    "stopRecordingTraffic",
    "addShardToZone",
    "removeShardFromZone",
    "oidcListKeys",
    "oidcRefreshKeys",
];

function runTests(tests, conn, impls, options) {
    for (const test of tests) {
        if (!denylistedTests.includes(test.testname)) {
            authCommandsLib.runOneTest(conn, test, impls, options);
        }
    }
}

const impls = {
    runOneTest: function(conn, testObj) {
        // Some tests requires mongot, however, setting this failpoint will make search queries to
        // return EOF, that way all the hassle of setting it up can be avoided.
        let disableSearchFailpoint;
        let mongosDisableSearchFailpoint;
        if (testObj.disableSearch) {
            disableSearchFailpoint = configureFailPoint(conn.rs0 ? conn.rs0.getPrimary() : conn,
                                                        'searchReturnEofImmediately');
            // In a sharded environment, the failpoint must be set on mongos and mongod.
            if (conn.s0) {
                mongosDisableSearchFailpoint =
                    configureFailPoint(conn.s0, 'searchReturnEofImmediately');
            }
        }
        const testCase = testObj.testcases[0];

        const runOnDb = conn.getDB(testCase.runOnDb);
        const state = testObj.setup && testObj.setup(runOnDb);

        let cmdDb = runOnDb;
        if (testObj.hasOwnProperty("runOnDb")) {
            assert.eq(typeof (testObj.runOnDb), "function");
            cmdDb = runOnDb.getSiblingDB(testObj.runOnDb(state));
        }

        const command = (typeof (testObj.command) === "function")
            ? testObj.command(state, testCase.commandArgs)
            : testObj.command;
        command['comment'] = {comment: true};
        const res = cmdDb.runCommand(command);
        assert(res.ok == 1 || testCase.expectFail || res.code == ErrorCodes.CommandNotSupported,
               tojson(res));

        if (testObj.teardown) {
            testObj.teardown(cmdDb, res);
        }

        if (disableSearchFailpoint) {
            disableSearchFailpoint.off();
        }
        if (mongosDisableSearchFailpoint) {
            mongosDisableSearchFailpoint.off();
        }
    }
};

let mongotmock;
let mongotHost = "localhost:27017";
if (!_isWindows()) {
    mongotmock = new MongotMock();
    mongotmock.start();
    mongotHost = mongotmock.getConnection().host;
}

// We have to set the mongotHost parameter for the $search-relatead tests to pass configuration
// checks.
const opts = {
    setParameter: {mongotHost}
};
let conn = MongoRunner.runMongod(opts);

// Test with standalone mongod.
runTests(tests, conn, impls);

MongoRunner.stopMongod(conn);

// Test with a sharded cluster. Some tests require the first shard's name acquired from the
// auth commands library to be up-to-date in order to set up correctly.
conn = new ShardingTest({shards: 1, mongos: 2, other: {rsOptions: opts, mongosOptions: opts}});
runTests(tests, conn, impls, {shard0name: conn.shard0.shardName});

conn.stop();

if (mongotmock) {
    mongotmock.stop();
}
