/**
 * Performs basic checks on the configureFailPoint command. Also check
 * mongo/util/fail_point_test.cpp for unit tests.
 *
 * @param adminDB {DB} the admin database database object
 */
var runTest = function(adminDB) {
    var expectFailPointState = function(fpState, expectedMode, expectedData) {
        assert.eq(expectedMode, fpState.mode);

        // Check that all expected data is present.
        for (var field in expectedData) {  // Valid only for 1 level field checks
            assert.eq(expectedData[field], fpState.data[field]);
        }

        // Check that all present data is expected.
        for (field in fpState.data) {
            assert.eq(expectedData[field], fpState.data[field]);
        }
    };

    // A failpoint's state can be read through getParameter by prefixing its name with "failpoint."

    // Test non-existing fail point
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'fpNotExist', mode: 'alwaysOn', data: {x: 1}}));

    // Test bad mode string
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'badMode', data: {x: 1}}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad mode obj
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: {foo: 3}, data: {x: 1}}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad mode type
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: true, data: {x: 1}}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test bad data type
    assert.commandFailed(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'alwaysOn', data: 'data'}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test setting mode to off.
    assert.commandWorked(adminDB.runCommand({configureFailPoint: 'dummy', mode: 'off'}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 0, {});

    // Test good command w/ data
    assert.commandWorked(
        adminDB.runCommand({configureFailPoint: 'dummy', mode: 'alwaysOn', data: {x: 1}}));
    var res = adminDB.runCommand({getParameter: 1, "failpoint.dummy": 1});
    assert.commandWorked(res);
    expectFailPointState(res["failpoint.dummy"], 1, {x: 1});
};

var conn = MongoRunner.runMongod();
runTest(conn.getDB('admin'));
MongoRunner.stopMongod(conn.port);

///////////////////////////////////////////////////////////
// Test mongos
var st = new ShardingTest({shards: 1});
runTest(st.s.getDB('admin'));
st.stop();
