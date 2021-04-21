/**
 * Verifies that the mongod logs a warning message for the following deprecated op codes and
 * command:
 * - OP_QUERY op code
 * - OP_GET_MORE op code
 * - OP_KILL_CURSORS op code
 * - OP_INSERT op code
 * - OP_DELETE op code
 * - OP_UPDATE op code
 * - getLastError command
 */

(function() {
"use strict";

load("jstests/noPassthrough/deprecated_wire_ops_lib.js");

const testHelper = new DeprecatedWireOpsTest();

/**
 * Starts up mongod with deprecatedWiresOpsWarningPeriodInSeconds='periodInSeconds' setParameter.
 * Returns an array of a connection to mongod and a test database object.
 */
const startUpMongod = (periodInSeconds = 3600) => {
    const paramDoc = testHelper.getParamDoc(periodInSeconds);
    const conn = MongoRunner.runMongod(paramDoc);
    assert.neq(conn, null, `mongod failed to start up with ${paramDoc}`);
    const testDB = conn.getDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    return [conn, testDB];
};

const tearDownMongod = (conn) => {
    MongoRunner.stopMongod(conn);
};

testHelper.runDeprecatedWireOpBasicLoggingBehaviorTest(() => {
    return testHelper.setUp(startUpMongod);
}, tearDownMongod);

testHelper.runLogAllDeprecatedWireOpsTest(() => {
    return testHelper.setUp(startUpMongod, /*periodInSeconds*/ 0);
}, tearDownMongod);

const periodInSeconds = 1;
testHelper.runDeprecatedWireOpPeriodTest((periodInSeconds) => {
    return testHelper.setUp(startUpMongod, periodInSeconds);
}, tearDownMongod, periodInSeconds);
})();
