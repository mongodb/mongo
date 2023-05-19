// Tests that OCSP responder latency is logged for client-side verification.
// @tags: [
//   requires_http_client,
// ]

load("jstests/ocsp/lib/mock_ocsp.js");

(function() {
"use strict";

// We only have custom logging output for openssl.
if (determineSSLProvider() !== "openssl") {
    return;
}

const mongodOptions = (connectionHealthLoggingOn) => {
    return {
        sslMode: "requireSSL",
        sslPEMKeyFile: OCSP_SERVER_CERT,
        sslCAFile: OCSP_CA_PEM,
        sslAllowInvalidHostnames: "",
        setParameter: {
            "failpoint.disableStapling": "{'mode':'alwaysOn'}",
            "ocspEnabled": "true",
            "enableDetailedConnectionHealthMetricLogLines": connectionHealthLoggingOn
        }
    };
};

let runTest = (options) => {
    const {ocspFaultType = "", connectionHealthLoggingOn} = options;

    let mock_ocsp = new MockOCSPServer("", 1);
    mock_ocsp.start();

    let conn = MongoRunner.runMongod(mongodOptions(connectionHealthLoggingOn));

    let loggingShellArg =
        `enableDetailedConnectionHealthMetricLogLines=${connectionHealthLoggingOn}`;

    clearRawMongoProgramOutput();
    // The desired log line will be printed by the shell. We run a parallel shell because
    // 'rawMongoProgramOutput' will only return logs for subprocesses spawned by the shell.
    let runParallelShellSuccess = startParallelShell(
        () => {
            jsTestLog(
                "Established connection with server to test successful certification verification.");
        },
        conn.port,
        null /*noConnect */,
        "--setShellParameter",
        loggingShellArg,
        "--tls",
        "--tlsCAFile",
        OCSP_CA_PEM,
        "--tlsCertificateKeyFile",
        OCSP_CLIENT_CERT,
        "--tlsAllowInvalidHostnames",
        "--verbose",
        1);
    runParallelShellSuccess();

    const successOutput = rawMongoProgramOutput();
    let failOutput;

    if (ocspFaultType != "") {
        mock_ocsp.stop();

        jsTestLog(`Restarting MockOCSPServer with ${ocspFaultType} option`);
        mock_ocsp = new MockOCSPServer(ocspFaultType, 1);
        mock_ocsp.start();

        clearRawMongoProgramOutput();

        assert.throws(startParallelShell(
            (ocspFaultType) => {
                jsTestLog("Something went wrong if we print this! Fault type: " + ocspFaultType);
            },
            conn.port,
            null /*noConnect */,
            "--setShellParameter",
            loggingShellArg,
            "--tls",
            "--tlsCAFile",
            OCSP_CA_PEM,
            "--tlsCertificateKeyFile",
            OCSP_CLIENT_CERT,
            "--tlsAllowInvalidHostnames",
            "--verbose",
            1));

        failOutput = rawMongoProgramOutput();
    }

    if (ocspFaultType == FAULT_REVOKED) {
        // Assert that the shell fails due to certificate being revoked, and we still measure
        // OCSP responder latency if logging is enabled.
        assert.gt(failOutput.search(/OCSPCertificateStatusRevoked/), 0);
    }

    // This is javascript's string search -- returns the first position of the regex string in
    // the source string if there is a match, else returns -1.
    if (connectionHealthLoggingOn) {
        assert.gte(successOutput.search(/"id":6840101/), 0, successOutput);
    } else {
        assert.eq(successOutput.search(/"id":6840101/), -1, successOutput);
    }

    if (failOutput) {
        if (connectionHealthLoggingOn) {
            assert.gte(failOutput.search(/"id":6840101/), 0, failOutput);
        } else {
            assert.eq(failOutput.search(/"id":6840101/), -1, failOutput);
        }
    }

    MongoRunner.stopMongod(conn);

    sleep(1000);
    mock_ocsp.stop();
};

runTest({connectionHealthLoggingOn: true});
runTest({connectionHealthLoggingOn: false});
runTest({ocspFaultType: FAULT_REVOKED, connectionHealthLoggingOn: true});
}());
