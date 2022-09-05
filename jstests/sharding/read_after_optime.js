// Test read after opTime functionality with maxTimeMS on config servers (CSRS only)`.

(function() {
'use strict';

var shardingTest = new ShardingTest({shards: 0});
assert(shardingTest.configRS, 'this test requires config servers to run in CSRS mode');

var configReplSetTest = shardingTest.configRS;
var primaryConn = configReplSetTest.getPrimary();

var lastOp = configReplSetTest.awaitLastOpCommitted();
assert(lastOp, 'invalid op returned from ReplSetTest.awaitLastOpCommitted()');

var config = configReplSetTest.getReplSetConfigFromNode();
var term = lastOp.t;

var runFindCommand = function(ts) {
    return primaryConn.getDB('local').runCommand({
        find: 'oplog.rs',
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

var pingIntervalSeconds = 10;
assert.commandFailedWithCode(
    runFindCommand(new Timestamp(lastOp.ts.getTime() + pingIntervalSeconds * 5, 0)),
    ErrorCodes.MaxTimeMSExpired);

var msg = /Command timed out waiting for read concern to be satisfied.*"db":"local"/;

assert.soon(function() {
    var logMessages = assert.commandWorked(primaryConn.adminCommand({getLog: 'global'})).log;
    for (var i = 0; i < logMessages.length; i++) {
        if (logMessages[i].search(msg) != -1) {
            return true;
        }
    }
    return false;
}, 'Did not see any log entries containing the following message: ' + msg, 60000, 300);
shardingTest.stop();
})();
