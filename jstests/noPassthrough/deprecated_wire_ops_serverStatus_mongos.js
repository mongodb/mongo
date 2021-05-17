/**
 * Verifies that the mongos accounts for the legacy opcodes in serverStatus.opcounters.
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
 * The opcounters test switches read and write mode to legacy for the mongos instance so it's better
 * to run the test in its own sandbox.
 */
const startUpMongos = () => {
    const st = new ShardingTest({mongos: 1, shards: 1, mongosOptions: {}});

    const testDB = st.s.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    return [st, testDB];
};

const tearDownMongos = (shardedCluster) => {
    shardedCluster.stop();
};

testHelper.runDeprecatedOpcountersInServerStatusTest(startUpMongos, tearDownMongos);
})();
