// Make sure that mongod fails to start up and logs an error
// when given an invalid hostname to bind to.

(function() {
    'use strict';

    const mongod = MongoRunner.runMongod(
        {bind_ip: "blargle.", nounixsocket: false, useLogFiles: true, waitForConnect: false});
    assert.soon(function() {
        try {
            const log = cat(mongod.fullOptions.logFile);
            return /No available addresses\/ports to bind to/.test(log);
        } catch (e) {
            return false;
        }
    }, "No log message found for bind_ip based startup failure.", 30 * 1000, 5 * 1000);
    MongoRunner.stopMongod(mongod, null, {allowedExitCode: MongoRunner.EXIT_NET_ERROR});
})();
