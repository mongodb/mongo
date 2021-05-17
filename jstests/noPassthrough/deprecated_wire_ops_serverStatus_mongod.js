/**
 * Verifies that the mongod accounts for the legacy opcodes in serverStatus.opcounters.
 */

(function() {
"use strict";

load("jstests/noPassthrough/deprecated_wire_ops_lib.js");

const testHelper = new DeprecatedWireOpsTest();

/**
 * The opcounters test switches read and write mode to legacy for the mongod instance so it's better
 * to run the test in its own sandbox.
 */
const startUpMongod = () => {
    const conn = MongoRunner.runMongod({});
    assert.neq(conn, null, `mongod failed to start up`);
    const testDB = conn.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    return [conn, testDB];
};

const tearDownMongod = (conn) => {
    MongoRunner.stopMongod(conn);
};

testHelper.runDeprecatedOpcountersInServerStatusTest(startUpMongod, tearDownMongod);
})();
