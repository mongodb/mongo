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
        testStartupLogging(launcher, makeRegExMatchFn(/waiting for connections on port/));
        launcher.stop(conn, undefined, {});
    }

    print("********************\nTesting startup logging in mongod\n********************");

    validateWaitingMessage({
        start: function(opts) {
            var actualOpts = {nojournal: ""};
            Object.extend(actualOpts, opts);
            return MongoRunner.runMongod(actualOpts);
        },
        stop: MongoRunner.stopMongod
    });

}());
