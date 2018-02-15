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

    function runTest(serverDisabledProtos, clientDisabledProtos) {
        let expectedRegex = /tlsv1 alert protocol version/;

        var md = MongoRunner.runMongod({
            nopreallocj: "",
            sslMode: "requireSSL",
            sslCAFile: "jstests/libs/ca.pem",
            sslPEMKeyFile: "jstests/libs/server.pem",
            sslDisabledProtocols: serverDisabledProtos,
            waitForConnect: false,
        });

        assert.soon(function() {
            clearRawMongoProgramOutput();
            let shell = runMongoProgram("mongo",
                                        "--port",
                                        md.port,
                                        ...clientOptions,
                                        "--sslDisabledProtocols",
                                        clientDisabledProtos);
            let mongoOutput = rawMongoProgramOutput();
            return mongoOutput.match(expectedRegex);
        });

        MongoRunner.stopMongod(md);
    }

    // Client recieves and reports a protocol version alert if it advertises a protocol older than
    // the server's oldest supported protocol
    runTest("TLS1_0", "TLS1_1,TLS1_2");
}());
