// Test server's adherence to serverAuth and clientAuth EKUs on X.509 certs.

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isMacOS} from "jstests/ssl/libs/ssl_helpers.js";

const kServerAuthClientCert = getX509Path("client_with_serverAuth_eku.pem");
const kBothEKUsClientCert = getX509Path("client_with_serverAuth_and_clientAuth_eku.pem");
const kNoEKUsClientCert = getX509Path("client_without_eku.pem");
const kClientAuthClientCert = getX509Path("client.pem");

const kClientAuthServerCert = getX509Path("server_with_clientAuth_eku.pem");
const kBothEKUsServerCert = getX509Path("server.pem");
const kNoEKUsServerCert = getX509Path("server_without_eku.pem");
const kServerAuthServerCert = getX509Path("server_with_serverAuth_eku.pem");

const kCACert = getX509Path("ca.pem");

function testClientAuthEKU(conn, clientCert, shouldFail) {
    clearRawMongoProgramOutput();
    const exitCode = runMongoProgram(
        "mongo",
        "--tls",
        "--tlsAllowInvalidHostnames",
        "--tlsCertificateKeyFile",
        clientCert,
        "--tlsCAFile",
        getX509Path("ca.pem"),
        "--port",
        conn.port,
        "--eval",
        ";",
    );

    let expectedFailureRegex = /unsuitable|unsupported certificate purpose/;

    if (isMacOS()) {
        expectedFailureRegex = /Certificate trust failure: Invalid Extended Key Usage for policy/;
    } else if (_isWindows()) {
        expectedFailureRegex = /The certificate is not valid for the requested usage./;
    }

    assert.soon(function () {
        const output = rawMongoProgramOutput(".*");
        clearRawMongoProgramOutput();

        const isRegexPresent = expectedFailureRegex.test(output);
        return (shouldFail && isRegexPresent) || (!shouldFail && !isRegexPresent);
    });
}

function testServerAuthEKU(serverCert, shouldFail) {
    const origSkipCheck = TestData.skipCheckDBHashes;
    const rst = new ReplSetTest({
        nodes: 2,
    });
    rst.startSet({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: serverCert,
        tlsCAFile: kCACert,
        tlsClusterFile: kBothEKUsServerCert,
        tlsAllowInvalidHostnames: "",
    });

    if (shouldFail) {
        const oldTimeout = ReplSetTest.kDefaultTimeoutMS;
        const shortTimeout = 5 * 1000;
        ReplSetTest.kDefaultTimeoutMS = shortTimeout;
        rst.timeoutMS = shortTimeout;
        MongoRunner.runHangAnalyzer.disable();
        try {
            assert.throws(function () {
                rst.initiate();
            });
        } finally {
            ReplSetTest.kDefaultTimeoutMS = oldTimeout;
            MongoRunner.runHangAnalyzer.enable();
        }
        TestData.skipCheckDBHashes = true;
    } else {
        rst.initiate();
        assert.commandWorked(rst.getPrimary().getDB("admin").runCommand({hello: 1}));
    }

    rst.stopSet();
    TestData.skipCheckDBHashes = origSkipCheck;
}

// clientAuth tests against standalone.
{
    const mongod = MongoRunner.runMongod({
        auth: "",
        tlsMode: "requireTLS",
        // Server PEM file is server.pem to match the shell's ca.pem.
        tlsCertificateKeyFile: getX509Path("server.pem"),
        tlsCAFile: getX509Path("ca.pem"),
        tlsAllowInvalidCertificates: "",
    });
    testClientAuthEKU(mongod, kClientAuthClientCert, false /* shouldFail */);
    testClientAuthEKU(mongod, kNoEKUsClientCert, false /* shouldFail */);
    testClientAuthEKU(mongod, kBothEKUsClientCert, false /* shouldFail */);
    testClientAuthEKU(mongod, kServerAuthClientCert, true /* shouldFail */);
    MongoRunner.stopMongod(mongod);
}

// serverAuth tests via replica set setup.
{
    testServerAuthEKU(kServerAuthServerCert, false /* shouldFail */);
    testServerAuthEKU(kBothEKUsServerCert, false /* shouldFail */);
    testServerAuthEKU(kClientAuthServerCert, true /* shouldFail */);

    // MacOS/Secure Transport's standard SSL cert verification policy is stricter than
    // Windows and OpenSSL in that it requires server certificates to include the serverAuth
    // EKU extension. Windows and OpenSSL accept server certificates that omit the EKU extension
    // entirely and only care that serverAuth is specified if any EKU exists.
    let shouldFailNoEKUsServerCert = false;
    if (isMacOS()) {
        shouldFailNoEKUsServerCert = true;
    }
    testServerAuthEKU(kNoEKUsServerCert, shouldFailNoEKUsServerCert /* shouldFail */);
}
