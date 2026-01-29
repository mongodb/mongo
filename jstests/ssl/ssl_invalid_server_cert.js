// Test invalid SSL keyfile settings.

import {requireSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

function runTest(name, config, expect) {
    jsTest.log("Running test: " + name);
    clearRawMongoProgramOutput();

    let mongod = null;
    try {
        mongod = MongoRunner.runMongod(config);
    } catch (e) {
        //
    }
    assert.eq(null, mongod, "Mongod started unexpectedly");

    const output = rawMongoProgramOutput(".*");
    assert.eq(true, output.includes(expect), "Server failure message did not include '" + expect + "'");
}

const validityMessage = "The provided SSL certificate is expired or not yet valid";

// Test that startup fails with certificate that has yet to become valid.
const notYetValid = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("not_yet_valid.pem"),
    tlsCAFile: getX509Path("ca.pem"),
};
runTest("not-yet-valid", notYetValid, validityMessage);

// Test that startup fails with expired certificate.
const expired = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("expired.pem"),
    tlsCAFile: getX509Path("ca.pem"),
};
runTest("expired", expired, validityMessage);

// Test that startup fails with no certificate at all.
const needKeyFile = "need tlsCertificateKeyFile or certificateSelector when TLS is enabled";
runTest("no-key-file", {tlsMode: "requireTLS", tlsCAFile: getX509Path("ca.pem")}, needKeyFile);

// Test that startup also fails if only tlsClusterFile is provided
runTest(
    "cluster-file-only",
    {
        tlsMode: "requireTLS",
        tlsCAFile: getX509Path("ca.pem"),
        tlsClusterFile: getX509Path("client.pem"),
    },
    needKeyFile,
);

requireSSLProvider(["windows", "apple"], function () {
    const selector = "subject=Trusted Kernel Test Server";

    // Test that startup also fails if only tlsClusterSelector is provided
    runTest(
        "cluster-selector-only",
        {
            tlsMode: "requireTLS",
            tlsCAFile: getX509Path("ca.pem"),
            tlsClusterCertificateSelector: selector,
        },
        needKeyFile,
    );

    // Test that startup fails if both key file and cert selector are provided
    const keyFileAndSelector = {
        tlsMode: "requireTLS",
        tlsCAFile: getX509Path("ca.pem"),
        tlsCertificateKeyFile: getX509Path("client.pem"),
        tlsCertificateSelector: selector,
    };
    runTest(
        "keyfile-and-selector",
        keyFileAndSelector,
        "net.tls.certificateKeyFile is not allowed when net.tls.certificateSelector is specified",
    );

    // Test that startup fails if both cluster file and cluster cert selector are provided
    const clusterFileAndSelector = {
        tlsMode: "requireTLS",
        tlsCAFile: getX509Path("ca.pem"),
        tlsClusterFile: getX509Path("client.pem"),
        tlsClusterCertificateSelector: selector,
    };
    runTest(
        "cluster-keyfile-and-selector",
        clusterFileAndSelector,
        "net.tls.clusterFile is not allowed when net.tls.clusterCertificateSelector is specified",
    );
});
