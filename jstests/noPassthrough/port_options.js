// Check --port= edge behaviors.

(function() {
    'use strict';
    jsTest.log("Setting port=0 is okay unless binding to multiple IP interfaces.");

    function runTest(bindIP, expectOk) {
        jsTest.log("".concat("Testing with bindIP=[", bindIP, "], expectOk=[", expectOk, "]"));

        clearRawMongoProgramOutput();

        let pid = startMongoProgramNoConnect(
            "mongod", "--ipv6", "--dbpath", MongoRunner.dataDir, "--bind_ip", bindIP, "--port", 0);
        jsTest.log("".concat("pid=[", pid, "]"));

        if (expectOk) {
            let port;

            // We use assert.soonNoExcept() here because the mongod may not be logging yet.
            assert.soonNoExcept(() => {
                const logContents = rawMongoProgramOutput();
                const found = logContents.match(/waiting for connections on port (\d+)/);
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
            assert.soonNoExcept(() => {
                const logContents = rawMongoProgramOutput();
                const found = logContents.match(
                    /Port 0 \(ephemeral port\) is not allowed when listening on multiple IP interfaces/);
                return (found !== null);
            }, "No warning issued for invalid port=0 usage");
        }
    }

    runTest("127.0.0.1", true);
    runTest("127.0.0.1,::1", false);
}());
