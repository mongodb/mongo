/**
 * Tests ingress connection metrics.
 *
 * @tags: [requires_fcv_60]
 */

"use strict";

(function() {
load("jstests/ssl/libs/ssl_helpers.js");

if (detectDefaultTLSProtocol() !== "TLS1_3") {
    // Short-circuit and exit test immediately -- before TLS 1.3, we can't use
    // opensslCipherSuiteConfig.
    jsTestLog("Platform does not support TLS 1.3; skipping test.");
    return;
}

// We use 'opensslCipherSuiteConfig' to deterministically set the cipher suite negotiated when
// openSSL is being used. This can be different on Windows/OSX implementations.
let cipherSuite = "TLS_AES_256_GCM_SHA384";

const tlsOptions = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
    setParameter: {opensslCipherSuiteConfig: cipherSuite},
};

// Get the logId that corresponds to the implementation of TLS being used.
let logId;
switch (determineSSLProvider()) {
    case "openssl":
        logId = 6723801;
        break;
    case "windows":
        logId = 6723802;
        // This cipher is chosen to represent the cipher negotiated by Windows Server 2019 by
        // default.
        cipherSuite = "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
        break;
    case "apple":
        logId = 6723803;
        // We log only the cipher represented as its enum value in this code path. This corresponds
        // to the hex value 0xC030 which maps to the cipher suite
        // "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384". This cipher is chosen by OSX 12.1 by default.
        cipherSuite = 49200;
        break;
    default:
        assert(false, "Failed to determine that we are using a supported SSL provider");
}

jsTestLog("Establishing connection to mongod");
const mongod = MongoRunner.runMongod(Object.merge(tlsOptions));
// Start a new connection to check that 'durationMicros' is cumulatively measured in server status
assert.soon(() => checkLog.checkContainsOnceJson(mongod, logId, {"cipher": cipherSuite}),
            "failed waiting for log line with negotiated cipher info");

MongoRunner.stopMongod(mongod);
}());
