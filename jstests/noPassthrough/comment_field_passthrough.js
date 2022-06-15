/**
 * Verify that adding 'comment' field to any command shouldn't cause unexpected failures.
 * @tags: [
 *   requires_capped,
 *   requires_persistence,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
(function() {

"use strict";

load("jstests/auth/lib/commands_lib.js");  // Provides an exhaustive list of commands.
load("jstests/libs/fail_point_util.js");   // Helper to enable/disable failpoints easily.

const tests = authCommandsLib.tests;

// The following commands require additional start up configuration and hence need to be skipped.
const denylistedTests =
    ["startRecordingTraffic", "stopRecordingTraffic", "addShardToZone", "removeShardFromZone"];

function runTests(tests, conn, impls) {
    const firstDb = conn.getDB(firstDbName);
    const secondDb = conn.getDB(secondDbName);
    const isMongos = authCommandsLib.isMongos(conn);
    for (const test of tests) {
        if (!denylistedTests.includes(test.testname)) {
            authCommandsLib.runOneTest(conn, test, impls, isMongos);
        }
    }
}

const impls = {
    runOneTest: function(conn, testObj) {
        // Some tests requires mongot, however, setting this failpoint will make search queries to
        // return EOF, that way all the hassle of setting it up can be avoided.
        let disableSearchFailpoint;
        if (testObj.disableSearch) {
            disableSearchFailpoint = configureFailPoint(conn.rs0 ? conn.rs0.getPrimary() : conn,
                                                        'searchReturnEofImmediately');
        }
        const testCase = testObj.testcases[0];

        const runOnDb = conn.getDB(testCase.runOnDb);
        const state = testObj.setup && testObj.setup(runOnDb);

        const command = (typeof (testObj.command) === "function")
            ? testObj.command(state, testCase.commandArgs)
            : testObj.command;
        command['comment'] = {comment: true};
        const res = runOnDb.runCommand(command);
        assert(res.ok == 1 || testCase.expectFail || res.code == ErrorCodes.CommandNotSupported,
               tojson(res));

        if (testObj.teardown) {
            testObj.teardown(runOnDb, res);
        }

        if (disableSearchFailpoint) {
            disableSearchFailpoint.off();
        }
    }
};

let conn = MongoRunner.runMongod();

// Test with standalone mongod.
runTests(tests, conn, impls);

MongoRunner.stopMongod(conn);

// Test with a sharded cluster. Some tests require the first shard's name acquired from the
// auth commands library to be up-to-date in order to set up correctly.
conn = new ShardingTest({shards: 1, mongos: 2});
shard0name = conn.shard0.shardName;
runTests(tests, conn, impls);

conn.stop();
})();
