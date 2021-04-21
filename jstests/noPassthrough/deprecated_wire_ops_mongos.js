/**
 * Verifies that the mongos logs a warning message for the following deprecated op codes and
 * command:
 * - OP_QUERY op code
 * - OP_GET_MORE op code
 * - OP_KILL_CURSORS op code
 * - OP_INSERT op code
 * - OP_DELETE op code
 * - OP_UPDATE op code
 * - getLastError command
 *
 * Without the following tag, this test suite fails on a linux-64-duroff build variant since the
 * build variant does not support sharding.
 * @tags: [requires_sharding]
 */

(function() {
"use strict";

load("jstests/noPassthrough/deprecated_wire_ops_lib.js");

const testHelper = new DeprecatedWireOpsTest();

/**
 * Starts up a sharded cluster with mongos
 * deprecatedWiresOpsWarningPeriodInSeconds='periodInSeconds' setParameter. Returns an array of a
 * sharded cluster object and a test database object.
 */
const startUpMongos = (periodInSeconds = 3600) => {
    const st = new ShardingTest(
        {mongos: 1, shards: 1, mongosOptions: testHelper.getParamDoc(periodInSeconds)});

    const testDB = st.s.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    return [st, testDB];
};

const tearDownMongos = (shardedCluster) => {
    shardedCluster.stop();
};

testHelper.runDeprecatedWireOpBasicLoggingBehaviorTest(() => {
    return testHelper.setUp(startUpMongos);
}, tearDownMongos);

testHelper.runLogAllDeprecatedWireOpsTest(() => {
    return testHelper.setUp(startUpMongos, /*periodInSeconds*/ 0);
}, tearDownMongos);

const periodInSeconds = 1;
testHelper.runDeprecatedWireOpPeriodTest((periodInSeconds) => {
    return testHelper.setUp(startUpMongos, periodInSeconds);
}, tearDownMongos, periodInSeconds);
})();
