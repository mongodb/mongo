/**
 * Tests that normal startup writes to the log files as expected.
 */

(function() {

'use strict';

function makeRegExMatchFn(pattern) {
    return function(text) {
        return pattern.test(text);
    };
}

function testStartupLogging(launcher, matchFn, expectedExitCode) {
    assert(matchFn(rawMongoProgramOutput()));
}

function validateWaitingMessage(launcher) {
    clearRawMongoProgramOutput();
    var conn = launcher.start({});
    launcher.stop(conn, undefined, {});
    testStartupLogging(
        launcher,
        makeRegExMatchFn(
            /"id":23016,"ctx":"listener","msg":"Waiting for connections","attr":{"port":/));
}

print("********************\nTesting startup logging in mongod\n********************");

validateWaitingMessage({
    start: function(opts) {
        return MongoRunner.runMongod(opts);
    },
    stop: MongoRunner.stopMongod
});
}());
