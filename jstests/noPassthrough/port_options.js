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
            let port;

            // We use assert.soonNoExcept() here because `cat(logpath)` may fail due to the mongod
            // not yet having created the log file yet.
            assert.soonNoExcept(() => {
                const found = cat(logpath).match(/waiting for connections on port (\d+)/);
                if (found !== null) {
                    print("Found message from mongod with port it is listening on: " + found[0]);
                    port = found[1];
                    return true;
                }
            });

            const connStr = `127.0.0.1:${port}`;
            print("Attempting to connect to " + connStr);

            let conn;
            assert.soonNoExcept(() => {
                conn = new Mongo(connStr);
                return true;
            });
            assert.commandWorked(conn.adminCommand({ping: 1}));

            stopMongoProgramByPid(pid);
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
