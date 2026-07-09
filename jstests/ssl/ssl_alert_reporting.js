// Ensure that TLS version alerts are correctly propagated

import {determineSSLProvider, sslProviderSupportsTLS1_1} from "jstests/ssl/libs/ssl_helpers.js";
import {windowsSupportsTLS13} from "jstests/libs/os_helpers.js";

const clientOptions = [
    "--tls",
    "--tlsCertificateKeyFile",
    getX509Path("client.pem"),
    "--tlsCAFile",
    getX509Path("ca.pem"),
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
        // Pick protocol-specific expectations for each scenario instead of using one broad regex.
        // TLS 1.3 mismatches:
        //  - server only TLS 1.3, client only TLS 1.2 -> Connection closed by peer
        //  - server only TLS 1.2, client only TLS 1.3 -> SEC_E_ALGORITHM_MISMATCH text
        // Legacy TLS 1.2 mismatch path:
        //  - Connection reset by peer
        if (serverDisabledProtos === "TLS1_2" && clientDisabledProtos === "TLS1_3") {
            expectedRegex =
                /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: .*Connection closed by peer/;
        } else if (serverDisabledProtos === "TLS1_3" && clientDisabledProtos === "TLS1_2") {
            expectedRegex =
                /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: .*cannot communicate, because they do not possess a common algorithm/;
        } else {
            expectedRegex =
                /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: .*Connection reset by peer/;
        }
    } else if (implementation === "apple") {
        expectedRegex =
            /Error: couldn't connect to server .*:[0-9]*, connection attempt failed: HostUnreachable: futurize.* Connection closed by peer.*/;
    } else {
        throw Error("Unrecognized TLS implementation!");
    }

    let md = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCAFile: getX509Path("ca.pem"),
        tlsCertificateKeyFile: getX509Path("server.pem"),
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
// Run TLS 1.3 scenarios on platforms that either disable TLS1.1 (legacy distros) or on
// Windows hosts that explicitly support TLS 1.3.
if (!sslProviderSupportsTLS1_1() || (determineSSLProvider() === "windows" && windowsSupportsTLS13())) {
    // Server disables TLS 1.2, client disables TLS 1.3
    runTest("TLS1_2", "TLS1_3");

    // Server disables TLS 1.3, client disables TLS 1.2
    runTest("TLS1_3", "TLS1_2");
} else {
    runTest("TLS1_0", "TLS1_1,TLS1_2");
}
