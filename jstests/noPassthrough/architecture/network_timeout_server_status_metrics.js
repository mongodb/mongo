/**
 *  @tags: [requires_fcv_63]
 *
 * Tests that metrics related to connection acquisition timeout are reported correctly
 * in serverStatus.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let collName = "testColl";
let numOps = 10;

let st = new ShardingTest({shards: 1, mongos: 1});
let testDB = st.s.getDB(jsTestName());
let coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({x: 1}));

const networkTimeoutLogID = 6496500;

function testConnectionTimeoutMetric(fpName, expectedNumOps, errorCode, networkTimeoutLog) {
    let prevTime = 0;
    let fp = configureFailPoint(st.s, fpName, {collectionNS: collName});
    // Run n find commands to check that the numConnectionNetworkTimeouts metric matches the
    // total # of ops issued.
    for (let i = 0; i < numOps; i++) {
        jsTestLog(`Issuing find command #${i}`);
        // Run a long query to make sure the network fail points fail the command.
        assert.commandFailedWithCode(testDB.runCommand({
            find: collName,
            filter: {
                $where:
                    "function() { const start = new Date().getTime(); while (new Date().getTime() - start < 100000); return true;}"
            }
        }),
                                     errorCode);
        if (networkTimeoutLog) {
            checkLog.containsJson(st.s, networkTimeoutLogID);
        }
        const opMetrics =
            assert.commandWorked(st.s.getDB("admin").serverStatus()).metrics.operation;
        let curTime = opMetrics.totalTimeWaitingBeforeConnectionTimeoutMillis;
        assert.gte(curTime, prevTime);
        prevTime = curTime;
        assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));
    }

    const opMetrics = assert.commandWorked(st.s.getDB("admin").serverStatus()).metrics.operation;
    assert.eq(expectedNumOps, opMetrics.numConnectionNetworkTimeouts, opMetrics);

    fp.off();
}

let maxRetries = 3;
// Run ops + 3 retries due to fail point induced retriable errors.
let expectedNumOps = numOps * (1 + maxRetries);

// This failpoint forces the connection acquisition timeout code
// PooledConnectionAcquisitionExceededTimeLimit to be returned. We test that connection
// acquisition timeout related metrics are recorded.
jsTestLog(`Using 'forceConnectionNetworkTimeout'.`);
testConnectionTimeoutMetric("forceConnectionNetworkTimeout",
                            expectedNumOps,
                            ErrorCodes.PooledConnectionAcquisitionExceededTimeLimit,
                            true);

// Test that a timeout not related to acquiring a connection will not cause the metrics to
// increment.
// This failpoint causes timeout and NetworkInterfaceExceededTimeLimit to be returned. This
// error code is not retriable, and will not increment connection network timeout metrics.
// Therefore, the `numConnectionNetworkTimeouts` metric should not change.
jsTestLog(`Using 'triggerSendRequestNetworkTimeout'.`);
testConnectionTimeoutMetric("triggerSendRequestNetworkTimeout",
                            expectedNumOps,
                            ErrorCodes.NetworkInterfaceExceededTimeLimit,
                            false);

st.stop();
