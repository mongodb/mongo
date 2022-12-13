/**
 *  @tags: [requires_fcv_63]
 *
 * Tests that operations that fail while waiting to acquire a connection will timeout with a
 * particular error code and that the number of times this occurs and the cumulative time spent
 * waiting prior to failure is properly recorded in serverStatus.
 */

load("jstests/libs/fail_point_util.js");

(function() {
"use strict";

let collName = "testColl";
let numOps = 10;

let st = new ShardingTest({shards: 1, mongos: 1});
let testDB = st.s.getDB(jsTestName());
let coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({x: 1}));

function testConnectionTimeoutMetric(fpName, logId, expectedNumOps) {
    let prevTime = 0;
    let fp = configureFailPoint(st.s, fpName, {collectionNS: collName});

    // Run n find commands to check that the totalTimeWaitingBeforeConnectionTimeoutMillis metric is
    // cumulative and the numConnectionNetworkTimeouts metric matches the total # of ops issued.
    for (let i = 0; i < numOps; i++) {
        jsTestLog(`Issuing find command #${i} using (${fpName}) failpoint`);
        assert.commandFailedWithCode(testDB.runCommand({"find": collName}),
                                     ErrorCodes.NetworkInterfaceExceededTimeLimit);
        checkLog.containsJson(st.s, logId);
        const opMetrics =
            assert.commandWorked(st.s.getDB("admin").serverStatus()).metrics.operation;
        let curTime = opMetrics.totalTimeWaitingBeforeConnectionTimeoutMillis;
        assert.gt(curTime, prevTime);
        prevTime = curTime;
        assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));
    }

    const opMetrics = assert.commandWorked(st.s.getDB("admin").serverStatus()).metrics.operation;
    assert.eq(expectedNumOps, opMetrics.numConnectionNetworkTimeouts, opMetrics);

    fp.off();
}

// This failpoint applies the NetworkInterfaceExceededTimeLimit error status on all connections
// taken from the connection pool.
testConnectionTimeoutMetric("forceConnectionNetworkTimeout", 6496500, numOps);
// This failpoint causes requests that have already obtained a working connection to time out right
// before sending is attempted.
testConnectionTimeoutMetric("triggerSendRequestNetworkTimeout", 6496501, numOps * 2);

st.stop();
})();
