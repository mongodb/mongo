/**
 * Verify that adding 'comment' field to any command shouldn't cause unexpected failures.
 * @tags: [
 *   requires_capped,
 *   requires_journaling,
 *   requires_persistence,
 *   requires_replication,
 *   requires_sharding,
 *   requires_wiredtiger,
 *   two_phase_index_builds_unsupported,
 * ]
 */
(function() {

"use strict";

load("jstests/auth/lib/commands_lib.js");  // Provides an exhaustive list of commands.

const tests = authCommandsLib.tests;

// The following commands require additional start up configuration and hence need to be skipped.
const blacklistedTests =
    ["startRecordingTraffic", "stopRecordingTraffic", "addShardToZone", "removeShardFromZone"];

function runTests(tests, conn, impls) {
    const firstDb = conn.getDB(firstDbName);
    const secondDb = conn.getDB(secondDbName);
    const isMongos = authCommandsLib.isMongos(conn);
    for (const test of tests) {
        if (!blacklistedTests.includes(test.testname)) {
            authCommandsLib.runOneTest(conn, test, impls, isMongos);
        }
    }
}

const impls = {
    runOneTest: function(conn, testObj) {
        const testCase = testObj.testcases[0];

        const runOnDb = conn.getDB(testCase.runOnDb);
        const state = testObj.setup && testObj.setup(runOnDb);

        const command = (typeof (testObj.command) === "function")
            ? testObj.command(state, testCase.commandArgs)
            : testObj.command;
        command['comment'] = "comment";
        const res = runOnDb.runCommand(command);
        assert(res.ok == 1 || testCase.expectFail || res.code == ErrorCodes.CommandNotSupported,
               tojson(res));

        if (testObj.teardown) {
            testObj.teardown(runOnDb, res);
        }
    }
};

let conn = MongoRunner.runMongod();

// Test with standalone mongod.
runTests(tests, conn, impls);

MongoRunner.stopMongod(conn);

conn = new ShardingTest({shards: 1, mongos: 2});

// Test with a sharded cluster.
runTests(tests, conn, impls);

conn.stop();
})();
