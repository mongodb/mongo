// Ensure that TLS version alerts are correctly propagated

load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    'use strict';

    const clientOptions = [
        "--ssl",
        "--sslPEMKeyFile",
        "jstests/libs/client.pem",
        "--sslCAFile",
        "jstests/libs/ca.pem",
        "--eval",
        ";"
    ];

    function runTest(transportLayer, serverDisabledProtos, clientDisabledProtos) {
        if (jsTest.options().serviceExecutor === "adaptive" && transportLayer !== "asio") {
            print("The transport layer is likely not compatible with this suite. Skipping...");
            return;
        }

        let expectedRegex = /tlsv1 alert protocol version/;

        var md = MongoRunner.runMongod({
            nopreallocj: "",
            sslMode: "requireSSL",
            sslCAFile: "jstests/libs/ca.pem",
            sslPEMKeyFile: "jstests/libs/server.pem",
            sslDisabledProtocols: serverDisabledProtos,
            transportLayer: transportLayer,
        });

        let shell;
        let mongoOutput;

        assert.soon(function() {
            clearRawMongoProgramOutput();
            shell = runMongoProgram("mongo",
                                    "--port",
                                    md.port,
                                    ...clientOptions,
                                    "--sslDisabledProtocols",
                                    clientDisabledProtos);
            mongoOutput = rawMongoProgramOutput();
            return mongoOutput.match(expectedRegex);
        }, "Mongo shell output was as follows:\n" + mongoOutput + "\n************");

        MongoRunner.stopMongod(md);
    }

    // Client recieves and reports a protocol version alert if it advertises a protocol older than
    // the server's oldest supported protocol
    runTest("asio", "TLS1_0", "TLS1_1,TLS1_2");
    runTest("legacy", "TLS1_0", "TLS1_1,TLS1_2");
}());
