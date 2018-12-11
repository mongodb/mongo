// Test keep-alive when using mongod's internal HttpClient.
// @tags: [requires_http_client]

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/configExpand/lib.js');

    function runTest(mongod, web) {
        assert(mongod);
        const admin = mongod.getDB('admin');

        // Only bother with this test when using curl >= 7.57.0.
        const http_status = admin.adminCommand({serverStatus: 1, http_client: 1});
        const http_client = assert.commandWorked(http_status).http_client;
        if (http_client.type !== 'curl') {
            print("*** Skipping test, not using curl");
            return;
        }

        printjson(http_client);
        if (http_client.running.version_num < 0x73900) {
            // 39 hex == 57 dec, so 0x73900 == 7.57.0
            print(
                "*** Skipping test, curl < 7.57.0 does not support connection pooling via share interface");
            return;
        }

        // Issue a series of requests to the mock server.
        for (let i = 0; i < 10; ++i) {
            const cmd =
                admin.runCommand({httpClientRequest: 1, uri: web.getStringReflectionURL(i)});
            const reflect = assert.commandWorked(cmd).body;
            assert.eq(reflect, i, "Mock server reflected something unexpected.");
        }

        // Check connect count.
        const countCmd =
            admin.runCommand({httpClientRequest: 1, uri: web.getURL() + '/connect_count'});
        const count = assert.commandWorked(countCmd).body;
        assert.eq(count, 1, "Connections were not kept alive.");

        // Force the open connection to close.
        const closeCmd =
            admin.runCommand({httpClientRequest: 1, uri: web.getURL() + '/connection_close'});
        const close = assert.commandWorked(closeCmd).body;
        assert.eq(close, 'closed');

        // Check count with new connection.
        const connectsCmd =
            admin.runCommand({httpClientRequest: 1, uri: web.getURL() + '/connect_count'});
        const connects = assert.commandWorked(connectsCmd).body;
        assert.eq(connects, 2, "Connection count incorrect.");
    }

    const web = new ConfigExpandRestServer();
    web.start();
    const mongod = MongoRunner.runMongod({setParameter: 'enableTestCommands=1'});
    runTest(mongod, web);
    MongoRunner.stopMongod(mongod);
    web.stop();
})();
