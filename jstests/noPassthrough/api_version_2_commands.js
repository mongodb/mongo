/**
 * Checks that API version 2 will behave correctly with mongod/mongos.
 *
 * @tags: [
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

const runTest = testDB => {
    // Test command in V2 but not V1.
    assert.commandWorked(testDB.runCommand({testVersion2: 1, apiVersion: "2", apiStrict: true}));
    assert.commandFailedWithCode(
        testDB.runCommand({testVersion2: 1, apiVersion: "1", apiStrict: true}),
        ErrorCodes.APIStrictError,
        "testVersion2 is not in API V1");

    // Test command in both V1 and V2.
    assert.commandWorked(
        testDB.runCommand({testVersions1And2: 1, apiVersion: "1", apiStrict: true}));
    assert.commandWorked(
        testDB.runCommand({testVersions1And2: 1, apiVersion: "2", apiStrict: true}));

    // Test command in V1, deprecated in V2.
    assert.commandWorked(testDB.runCommand({
        testDeprecationInVersion2: 1,
        apiVersion: "1",
        apiStrict: true,
        apiDeprecationErrors: true
    }));
    assert.commandFailedWithCode(
        testDB.runCommand({
            testDeprecationInVersion2: 1,
            apiVersion: "2",
            apiStrict: true,
            apiDeprecationErrors: true
        }),
        ErrorCodes.APIDeprecationError,
        "Provided apiDeprecationErrors: true, but testDeprecationInVersion2 is deprecated in V2");

    // Test command in V1, removed in V2.
    assert.commandWorked(testDB.runCommand({testRemoval: 1, apiVersion: "1", apiStrict: true}));
    assert.commandFailedWithCode(
        testDB.runCommand({testRemoval: 1, apiVersion: "2", apiStrict: true}),
        ErrorCodes.APIStrictError,
        "testRemoval is not in API V2");
};

const conn = MongoRunner.runMongod({setParameter: {acceptApiVersion2: true}});
const db = conn.getDB(jsTestName());
runTest(db);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({mongosOptions: {setParameter: {acceptApiVersion2: true}}});
runTest(st.s0.getDB(jsTestName()));
st.stop();
})();
