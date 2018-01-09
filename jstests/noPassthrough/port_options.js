// Check --port= edge behaviors.

(function() {
    'use strict';
    jsTest.log("Setting port=0 is okay unless binding to multiple IP interfaces.");

    function runTest(bindIP, expectOk) {
        jsTest.log("".concat("Testing with bindIP=[", bindIP, "], expectOk=[", expectOk, "]"));

        const logpath = "".concat(MongoRunner.dataDir, "/mongod.log");

        let pid = startMongoProgramNoConnect("mongod",
                                             "--ipv6",
                                             "--dbpath",
                                             MongoRunner.dataDir,
                                             "--logpath",
                                             logpath,
                                             "--bind_ip",
                                             bindIP,
                                             "--port",
                                             0);
        jsTest.log("".concat("pid=[", pid, "]"));

        if (expectOk) {
            const ec = stopMongoProgramByPid(pid);
            const expect = _isWindows() ? 1 : -15;  // SIGTERM is 15
            assert.eq(ec, expect, "Expected mongod terminate");
        } else {
            const ec = waitProgram(pid);
            assert.eq(ec, MongoRunner.EXIT_NET_ERROR);
            assert(
                /Port 0 \(ephemeral port\) is not allowed when listening on multiple IP interfaces/
                    .test(cat(logpath)),
                "No warning issued for invalid port=0 usage");
        }
    }
    runTest("127.0.0.1", true);
    runTest("127.0.0.1,::1", false);
}());
