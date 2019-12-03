// @tags: [requires_sharding]

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

/**
 * Performs basic checks on the configureFailPoint and waitForFailPoint command.
 * Also check mongo/util/fail_point_test.cpp for unit tests.
 *
 * @param adminDB {DB} the admin database database object
 */
function runBasicTest(adminDB) {
    function expectFailPointState(fpState, expectedMode, expectedData) {
        assert.eq(expectedMode, fpState.mode);

        // Check that all expected data is present.
        for (var field in expectedData) {  // Valid only for 1 level field checks
            assert.eq(expectedData[field], fpState.data[field]);
        }

        // Check that all present data is expected.
        for (field in fpState.data) {
            assert.eq(expectedData[field], fpState.data[field]);
        }
    }

    var res;

    // A failpoint's state can be read through getParameter by prefixing its name with
    // "failpoint"

    // Test non-existing fail point
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'fpNotExist', mode: 'alwaysOn', data: {x: 1}}));

    // Test bad mode string
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'badMode', data: {x: 1}}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad mode obj
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: {foo: 3}, data: {x: 1}}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad mode type
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: true, data: {x: 1}}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad data type
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'alwaysOn', data: 'data'}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test setting mode to off.
    assert.commandWorked(adminDB.runCommand({configureFailPoint: 'dummy', mode: 'off'}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test setting mode to skip.
    assert.commandWorked(adminDB.runCommand({configureFailPoint: 'dummy', mode: {skip: 2}}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 4, {});

    // Test good command w/ data
    assert.commandWorked(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'alwaysOn', data: {x: 1}}));
    res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 1, {x: 1});

    // Test that the timeout for waitForFailPoint can be set via maxTimeMS.
    var configureFailPointRes = adminDB.runCommand({configureFailPoint: 'dummy', mode: 'alwaysOn'});
    assert.commandWorked(configureFailPointRes);
    assert.commandFailedWithCode(adminDB.adminCommand({
        waitForFailPoint: "dummy",
        timesEntered: configureFailPointRes.count + 1,
        maxTimeMS: 10
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Test that waitForFailPoint throws an error when maxTimeMS is not provided.
    assert.commandFailedWithCode(adminDB.adminCommand({waitForFailPoint: "dummy", timesEntered: 1}),
                                 40414);
}

// Test the parameter handling.
var conn = MongoRunner.runMongod();
runBasicTest(conn.getDB('admin'));
MongoRunner.stopMongod(conn);

var st = new ShardingTest({shards: 1});
runBasicTest(st.s.getDB('admin'));

// Test the functionality of the commands.
const testDB = st.shard0.getDB("test");
const testColl = testDB["user"];
const failPointName = "hangAfterCollectionInserts";

// Turn on the fail point and check that the returned count is 0.
var configureFailPointRes = assert.commandWorked(testDB.adminCommand({
    configureFailPoint: failPointName,
    mode: "alwaysOn",
    data: {collectionNS: testColl.getFullName()}
}));
assert.eq(0, configureFailPointRes.count);

const joinHungWrite = startParallelShell(() => {
    assert.commandWorked(db.getSiblingDB("test").user.insert({_id: 0}));
}, st.rs0.getPrimary().port);

// Wait for the fail point to be entered.
assert.commandWorked(testDB.adminCommand({
    waitForFailPoint: failPointName,
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Turn off the fail point
configureFailPointRes =
    assert.commandWorked(testDB.adminCommand({configureFailPoint: failPointName, mode: "off"}));
assert.lte(1, configureFailPointRes.count);

joinHungWrite();

st.stop();
})();
