/*
 * Helper functions which connect to a server, and check its logs for particular strings.
 */
var checkLog;

(function() {
    "use strict";

    if (checkLog) {
        return;  // Protect against this file being double-loaded.
    }

    checkLog = (function() {
        /*
         * Calls the 'getLog' function at regular intervals on the provided connection 'conn' until
         * the provided 'msg' is found in the logs, or 5 minutes have elapsed. Throws an exception
         * on timeout.
         */
        var contains = function(conn, msg) {
            assert.soon(
                function() {
                    var logMessages =
                        assert.commandWorked(conn.adminCommand({getLog: 'global'})).log;
                    for (var i = 0; i < logMessages.length; i++) {
                        if (logMessages[i].indexOf(msg) != -1) {
                            return true;
                        }
                    }
                    return false;
                },
                'Could not find log entries containing the following message: ' + msg,
                5 * 60 * 1000,
                300);
        };

        /*
         * Calls the 'getLog' function at regular intervals on the provided connection 'conn' until
         * the provided 'msg' is found in the logs exactly 'expectedCount' times, or 5 minutes have
         * elapsed.
         * Throws an exception on timeout.
         */
        var containsWithCount = function(conn, msg, expectedCount) {
            var count = 0;
            assert.soon(
                function() {
                    var logMessages =
                        assert.commandWorked(conn.adminCommand({getLog: 'global'})).log;
                    for (var i = 0; i < logMessages.length; i++) {
                        if (logMessages[i].indexOf(msg) != -1) {
                            count++;
                        }
                    }

                    return expectedCount === count;
                },
                'Expected ' + expectedCount + ', but instead saw ' + count +
                    ' log entries containing the following message: ' + msg,
                5 * 60 * 1000,
                300);
        };

        return {contains: contains, containsWithCount: containsWithCount};
    })();
})();
