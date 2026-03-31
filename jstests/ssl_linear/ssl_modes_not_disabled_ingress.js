// Test that tlsMode="{allow|prefer|require}TLS" are correctly enforced on
// ingress connections accepted by both TCP/IP and Unix Domain sockets.

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {runTLSModeTest} from "jstests/ssl/libs/ssl_helpers.js";

if (_isWindows()) {
    quit();
}

const HOST_TYPE = getBuildInfo().buildEnvironment.target_os;
jsTest.log.info("HOST_TYPE = " + HOST_TYPE);

let trustedCA = getX509Path("trusted-ca.pem");
let trustedServer = getX509Path("trusted-server.pem");
let trustedClient = getX509Path("trusted-client.pem");

if (HOST_TYPE == "macOS") {
    trustedCA = "/opt/x509/macos-trusted-ca.pem";
    trustedServer = "/opt/x509/macos-trusted-server.pem";
    trustedClient = "/opt/x509/macos-trusted-client.pem";
    // Ensure trustedCA is properly installed on MacOS hosts.
    // (MacOS is the only OS where it is installed outside of this test)
    let exitCode = runProgram("security", "verify-cert", "-c", trustedClient);
    assert.eq(0, exitCode, "Check for proper installation of Trusted CA on MacOS host");
}

// jstests/sslSpecial/ssl_modes_disabled_ingress.js covers 'disabled'. We can't
// include all of them in one file because ssl_special doesn't support requireTLS while
// ssl_linear doesn't support disabled.
const socketPrefix = `${MongoRunner.dataDir}/socketdir`;
mkdir(socketPrefix);

function runTestWithMode(tlsMode) {
    const mongod = MongoRunner.runMongod(tlsMode);
    runTLSModeTest(mongod, tlsMode.tlsMode, trustedClient, trustedCA, socketPrefix);
    MongoRunner.stopMongod(mongod);

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            keyFile: "jstests/libs/key1",
            configOptions: tlsMode,
            mongosOptions: tlsMode,
            rsOptions: tlsMode,
            useHostname: false,
        },
    });
    runTLSModeTest(st.s0, tlsMode.tlsMode, trustedClient, trustedCA, socketPrefix);
    st.stop();
}

const requireTLSMode = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: trustedServer,
    tlsCAFile: trustedCA,
    tlsClusterFile: trustedClient,
    unixSocketPrefix: socketPrefix,
};

const allowTLSMode = {
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: trustedServer,
    tlsCAFile: trustedCA,
    tlsClusterFile: trustedClient,
    unixSocketPrefix: socketPrefix,
};

const preferTLSMode = {
    tlsMode: "preferTLS",
    tlsCertificateKeyFile: trustedServer,
    tlsCAFile: trustedCA,
    tlsClusterFile: trustedClient,
    unixSocketPrefix: socketPrefix,
};

runTestWithMode(requireTLSMode);
runTestWithMode(allowTLSMode);
runTestWithMode(preferTLSMode);
