// Ensure that TLS version alerts are correctly propagated

import {determineSSLProvider, sslProviderSupportsTLS1_1} from "jstests/ssl/libs/ssl_helpers.js";

const clientOptions = [
    "--tls",
    "--tlsCertificateKeyFile",
    "jstests/libs/client.pem",
    "--tlsCAFile",
    "jstests/libs/ca.pem",
    "--eval",
    ";",
];

function runTest(serverDisabledProtos, clientDisabledProtos) {
    const implementation = determineSSLProvider();
    let expectedRegex;
    // OpenSSL 1.0.2 and earlier versions don't emit the TLSv1 alert protocol message. We need to
    // account for both OpenSSL 3.0 and older OpenSSL versions. Debian 12 emits "tlsv1 alert
    // internal error"
    if (implementation === "openssl") {
        expectedRegex =
            /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: SocketException: .*(tlsv1 alert protocol version|tlsv1 alert internal error|short read)/;
    } else if (implementation === "windows") {
        expectedRegex =
            /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: .*Connection reset by peer/;
    } else if (implementation === "apple") {
        expectedRegex =
            /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: HostUnreachable: futurize.* Connection closed by peer.*/;
    } else {
        throw Error("Unrecognized TLS implementation!");
    }

    let md = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCAFile: "jstests/libs/ca.pem",
        tlsCertificateKeyFile: "jstests/libs/server.pem",
        tlsDisabledProtocols: serverDisabledProtos,
    });

    let mongoOutput;

    assert.soon(
        function () {
            clearRawMongoProgramOutput();
            runMongoProgram(
                "mongo",
                "--port",
                md.port,
                ...clientOptions,
                "--tlsDisabledProtocols",
                clientDisabledProtos,
            );
            mongoOutput = rawMongoProgramOutput(".*");
            return mongoOutput.match(expectedRegex);
        },
        "Mongo shell output was as follows:\n" + mongoOutput + "\n************",
        60 * 1000,
    );

    MongoRunner.stopMongod(md);
}

// Client receives and reports a protocol version alert if it advertises a protocol older than
// the server's oldest supported protocol
if (!sslProviderSupportsTLS1_1()) {
    // On platforms that disable TLS 1.1, assume they have TLS 1.3 for this test.
    // Server disables TLS 1.2, client disables TLS 1.3
    runTest("TLS1_2", "TLS1_3");

    // Server disables TLS 1.3, client disables TLS 1.2
    runTest("TLS1_3", "TLS1_2");
} else {
    runTest("TLS1_0", "TLS1_1,TLS1_2");
}
