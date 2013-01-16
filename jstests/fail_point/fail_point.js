/**
 * Performs basic checks on the failpoint command. Also check
 * mongo/util/fail_point_test.cpp for unit tests.
 *
 * @param adminDB {DB} the admin database database object
 */
var runTest = function(adminDB) {
    /**
     * Checks whether the result object from the configureFailPoint command
     * matches what we expect.
     *
     * @param resultObj {Object}
     * @param expectedMode {Number}
     * @param expectedData {Object}
     */
    var expectedFPState = function(resultObj, expectedMode, expectedData) {
        assert(resultObj.ok);
        assert.eq(expectedMode, resultObj.mode);

        // Valid only for 1 level field checks
        for (var field in expectedData) {
            assert.eq(expectedData[field], resultObj.data[field]);
        }

        for (field in resultObj.data) {
            assert.eq(expectedData[field], resultObj.data[field]);
        }
    };

    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 0, {});

    // Test non-existing fail point
    assert.commandFailed(adminDB.runCommand({ configureFailPoint: 'fpNotExist',
        mode: 'alwaysOn', data: { x: 1 }}));

    // Test bad mode string
    assert.commandFailed(adminDB.runCommand({ configureFailPoint: 'dummy',
        mode: 'madMode', data: { x: 1 }}));
    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 0, {});

    // Test bad mode obj
    assert.commandFailed(adminDB.runCommand({ configureFailPoint: 'dummy',
        mode: { foo: 3 }, data: { x: 1 }}));
    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 0, {});

    // Test bad mode type
    assert.commandFailed(adminDB.runCommand({ configureFailPoint: 'dummy',
        mode: true, data: { x: 1 }}));
    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 0, {});

    // Test bad data type
    assert.commandFailed(adminDB.runCommand({ configureFailPoint: 'dummy',
        mode: 'alwaysOn', data: 'data'}));
    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 0, {});

    // Test good command w/ data
    assert.commandWorked(adminDB.runCommand({ configureFailPoint: 'dummy',
        mode: 'alwaysOn', data: { x: 1 }}));
    expectedFPState(adminDB.runCommand({ configureFailPoint: 'dummy' }), 1, { x: 1 });
};

var conn = MongoRunner.runMongod({ port: 29000 });
runTest(conn.getDB('admin'));
MongoRunner.stopMongod(conn.port);

///////////////////////////////////////////////////////////
// Test mongos
var st = new ShardingTest({ shards: 1 });
runTest(st.s.getDB('admin'));
st.stop();

