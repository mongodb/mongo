/**
 * Tests ingress connection metrics.
 *
 * @tags: [requires_fcv_63]
 */

"use strict";

(function() {
load("jstests/ssl/libs/ssl_helpers.js");

// Short circuits for system configurations that do not support this setParameter, (i.e. OpenSSL
// versions that don't support TLS 1.3)
if (determineSSLProvider() === "openssl" && detectDefaultTLSProtocol() !== "TLS1_3") {
    jsTestLog("Platform does not support TLS 1.3; skipping test.");
    return;
}

// We use 'opensslCipherSuiteConfig' to deterministically set the cipher suite negotiated when
// openSSL is being used. This can be different on Windows/OSX implementations.
let cipherSuite = "TLS_AES_256_GCM_SHA384";

const mongodOptions = (connectionHealthLoggingOn) => {
    let options = {
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: "jstests/libs/server.pem",
        tlsCAFile: "jstests/libs/ca.pem",
        setParameter: {
            opensslCipherSuiteConfig: cipherSuite,
            enableDetailedConnectionHealthMetricLogLines: connectionHealthLoggingOn
        },
    };

    return options;
};

function testConn(mongod) {
    const mongo = runMongoProgram('mongo',
                                  '--host',
                                  'localhost',
                                  '--port',
                                  mongod.port,
                                  '--tls',
                                  '--tlsCAFile',
                                  'jstests/libs/ca.pem',
                                  '--tlsCertificateKeyFile',
                                  'jstests/libs/client.pem',
                                  '--eval',
                                  ';');
    return mongo === 0;
}

let runTest = (connectionHealthLoggingOn) => {
    jsTestLog("Establishing connection to mongod");
    let mongod = MongoRunner.runMongod(Object.merge(mongodOptions(connectionHealthLoggingOn)));
    let ssNetworkMetrics = mongod.adminCommand({serverStatus: 1}).metrics.network;
    let initialHandshakeTimeMillis = ssNetworkMetrics.totalIngressTLSHandshakeTimeMillis;
    jsTestLog(`totalTLSHandshakeTimeMillis: ${initialHandshakeTimeMillis}`);

    if (connectionHealthLoggingOn) {
        checkLog.containsJson(
            mongod, 6723804, {durationMillis: Number(initialHandshakeTimeMillis)});
    } else {
        assert.eq(checkLog.checkContainsOnceJson(mongod, 6723804, {}), false);
    }

    assert.commandWorked(mongod.adminCommand({clearLog: 'global'}));
    assert.eq(1, ssNetworkMetrics.totalIngressTLSConnections, ssNetworkMetrics);

    // Get the logId that corresponds to the implementation of TLS being used.
    let logId;
    switch (determineSSLProvider()) {
        case "openssl":
            logId = 6723801;
            break;
        case "windows":
            logId = 6723802;
            // This cipher is chosen to represent the cipher negotiated by Windows Server 2019
            // by default.
            cipherSuite = "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
            break;
        case "apple":
            logId = 6723803;
            // We log only the cipher represented as its enum value in this code path. This
            // corresponds to the hex value 0xC030 which maps to the cipher suite
            // "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384". This cipher is chosen by OSX 12.1 by
            // default.
            cipherSuite = 49200;
            break;
        default:
            assert(false, "Failed to determine that we are using a supported SSL provider");
    }

    // Start a new connection to check that 'durationMicros' is cumulatively measured in server
    // status.
    assert.soon(() => testConn(mongod), "Couldn't connect to mongod");
    ssNetworkMetrics = mongod.adminCommand({serverStatus: 1}).metrics.network;
    let totalTLSHandshakeTimeMillis = ssNetworkMetrics.totalIngressTLSHandshakeTimeMillis;
    jsTestLog(`totalTLSHandshakeTimeMillis: ${totalTLSHandshakeTimeMillis}`);
    let secondHandshakeDuration = totalTLSHandshakeTimeMillis - initialHandshakeTimeMillis;

    if (connectionHealthLoggingOn) {
        checkLog.containsJson(mongod, 6723804, {durationMillis: Number(secondHandshakeDuration)});
        assert.soon(() => checkLog.checkContainsOnceJson(mongod, logId, {"cipher": cipherSuite}),
                    "failed waiting for log line with negotiated cipher info");
    } else {
        assert.eq(checkLog.checkContainsOnceJson(mongod, 6723804, {}), false);
        assert.eq(checkLog.checkContainsOnceJson(mongod, logId, {}), false);
    }

    assert.gt(totalTLSHandshakeTimeMillis, initialHandshakeTimeMillis);
    assert.eq(2, ssNetworkMetrics.totalIngressTLSConnections, ssNetworkMetrics);

    MongoRunner.stopMongod(mongod);
};

// Parameterized on turning connection health logging on/off.
runTest(true);
runTest(false);
}());
