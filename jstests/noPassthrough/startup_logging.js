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
        assert(matchFn(rawMerizoProgramOutput()));
    }

    function validateWaitingMessage(launcher) {
        clearRawMerizoProgramOutput();
        var conn = launcher.start({});
        launcher.stop(conn, undefined, {});
        testStartupLogging(launcher, makeRegExMatchFn(/waiting for connections on port/));
    }

    print("********************\nTesting startup logging in merizod\n********************");

    validateWaitingMessage({
        start: function(opts) {
            return MerizoRunner.runMerizod(opts);
        },
        stop: MerizoRunner.stopMerizod
    });

}());
