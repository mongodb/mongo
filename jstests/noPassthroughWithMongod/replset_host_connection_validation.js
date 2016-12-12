// Test --host with a replica set.
(function() {

    const replSetName = 'hostTestReplSetName';

    // This "inner_mode" method of spawning a replset and re-running was copied from
    // host_connection_string_validation.js
    if ("undefined" == typeof inner_mode) {
        jsTest.log("Outer mode test starting a replica set");

        const replTest = new ReplSetTest({name: replSetName, nodes: 2});
        replTest.startSet();
        replTest.initiate();

        const primary = replTest.getPrimary();

        const args = [
            "mongo",
            "--nodb",
            "--eval",
            "inner_mode=true;port=" + primary.port + ";",
            "jstests/noPassthroughWithMongod/host_connection_string_validation.js"
        ];
        const exitCode = _runMongoProgram(...args);
        jsTest.log("Inner mode test finished, exit code was " + exitCode);

        // Stop the server we started
        jsTest.log("Outer mode test stopping server");
        MongoRunner.stopMongod(primary.port, 15);

        // Pass the inner test's exit code back as the outer test's exit code
        quit(exitCode);
    }

    const testHost = function(host) {
        const exitCode = runMongoProgram('mongo', '--eval', ';', '--host', host);
        if (exitCode !== 0) {
            doassert("failed to connect with `--host " + host +
                     "`, but expected success. Exit code: " + exitCode);
        }
    };

    const connStrings = [
        `localhost:${port}`,
        `${replSetName}/localhost:${port}`,
        `mongodb://localhost:${port}/admin?replicaSet=${replSetName}`,
        `mongodb://localhost:${port}`,
    ];

    function runConnectionStringTestFor(i, connectionString) {
        print("Testing connection string " + i + "...");
        print("    * testing " + connectionString);
        testHost(connectionString);
    }

    for (let i = 0; i < connStrings.length; ++i) {
        runConnectionStringTestFor(i, connStrings[i]);
    }

    jsTest.log("SUCCESSFUL test completion");
})();
