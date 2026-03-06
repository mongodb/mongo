// Test that tlsMode="{allow|prefer|require}TLS" are correctly enforced on
// ingress connections accepted by both TCP/IP and Unix Domain sockets.

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runTLSModeTest,
    TRUSTED_CA_CERT,
    TRUSTED_CLIENT_CERT,
    TRUSTED_CLUSTER_CERT,
    TRUSTED_SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

if (_isWindows()) {
    quit();
}

// jstests/sslSpecial/ssl_modes_disabled_ingress.js covers 'disabled'. We can't
// include all of them in one file because ssl_special doesn't support requireTLS while
// ssl_linear doesn't support disabled.
const socketPrefix = `${MongoRunner.dataDir}/socketdir`;
mkdir(socketPrefix);

function runTestWithMode(tlsMode) {
    const mongod = MongoRunner.runMongod(tlsMode);
    runTLSModeTest(mongod, tlsMode.tlsMode, TRUSTED_CLIENT_CERT, TRUSTED_CA_CERT, socketPrefix);
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
    runTLSModeTest(st.s0, tlsMode.tlsMode, TRUSTED_CLIENT_CERT, TRUSTED_CA_CERT, socketPrefix);
    st.stop();
}

const requireTLSMode = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: TRUSTED_SERVER_CERT,
    tlsCAFile: TRUSTED_CA_CERT,
    tlsClusterFile: TRUSTED_CLUSTER_CERT,
    unixSocketPrefix: socketPrefix,
};

const allowTLSMode = {
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: TRUSTED_SERVER_CERT,
    tlsCAFile: TRUSTED_CA_CERT,
    tlsClusterFile: TRUSTED_CLUSTER_CERT,
    unixSocketPrefix: socketPrefix,
};

const preferTLSMode = {
    tlsMode: "preferTLS",
    tlsCertificateKeyFile: TRUSTED_SERVER_CERT,
    tlsCAFile: TRUSTED_CA_CERT,
    tlsClusterFile: TRUSTED_CLUSTER_CERT,
    unixSocketPrefix: socketPrefix,
};

runTestWithMode(requireTLSMode);
runTestWithMode(allowTLSMode);
runTestWithMode(preferTLSMode);
