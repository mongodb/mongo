// Test read after opTime functionality with maxTimeMS on config servers (CSRS only)`.

import {ShardingTest} from "jstests/libs/shardingtest.js";

let shardingTest = new ShardingTest({shards: TestData.configShard ? 1 : 0});

assert(shardingTest.configRS, "this test requires config servers to run in CSRS mode");

let configReplSetTest = shardingTest.configRS;
let primaryConn = configReplSetTest.getPrimary();

let lastOp = configReplSetTest.awaitLastOpCommitted();
assert(lastOp, "invalid op returned from ReplSetTest.awaitLastOpCommitted()");

let term = lastOp.t;

let runFindCommand = function (ts) {
    return primaryConn.getDB("local").runCommand({
        find: "oplog.rs",
        readConcern: {
            afterOpTime: {
                ts: ts,
                t: term,
            },
        },
        maxTimeMS: 5000,
    });
};

assert.commandWorked(runFindCommand(lastOp.ts));

let pingIntervalSeconds = 10;
assert.commandFailedWithCode(
    runFindCommand(new Timestamp(lastOp.ts.getTime() + pingIntervalSeconds * 5, 0)),
    ErrorCodes.MaxTimeMSExpired,
);

let msg = /Command timed out waiting for read concern to be satisfied.*"db":"local"/;

assert.soon(
    function () {
        let logMessages = assert.commandWorked(primaryConn.adminCommand({getLog: "global"})).log;
        for (let i = 0; i < logMessages.length; i++) {
            if (logMessages[i].search(msg) != -1) {
                return true;
            }
        }
        return false;
    },
    "Did not see any log entries containing the following message: " + msg,
    60000,
    300,
);
shardingTest.stop();
