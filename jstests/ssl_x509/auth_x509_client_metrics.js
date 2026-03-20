/**
 * Tests that client-side SASL authentication using X.509 produces the expected connection health
 * log lines.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const kClientSaslX509SuccessLogId = 10748708;

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    tlsClusterFile: getX509Path("cluster_cert.pem"),
    tlsAllowInvalidHostnames: "",
    clusterAuthMode: "x509",
    useLogFiles: true,
};

// Startup of an auth-enabled sharded cluster with clusterAuthMode=x509 will use MONGODB-X509 for
// intra-cluster auth, we will validate corresponding logs
const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    keyFile: "jstests/libs/key1",
    other: {
        configOptions: x509_options,
        mongosOptions: x509_options,
        rsOptions: x509_options,
        useHostname: false,
    },
});

jsTest.log.info("Check secondary config servers for authentication logs");
st.configRS.getSecondaries().forEach((conn) => {
    // getLog pulls a maximum of 1024 entries, which is sometimes not enough to find intra-cluster
    // auth logs because startup is noisy. Instead, we search the log file directly.
    assert.soon(
        () =>
            checkLog.checkContainsWithAtLeastCountJson(
                conn.fullOptions.logFile,
                kClientSaslX509SuccessLogId,
                {
                    "subjectName": "CN=clustertest,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
                    "targetDatabase": "$external",
                    "mechanism": "MONGODB-X509",
                    "result": 0,
                    "metrics": {
                        "conversation_duration": {
                            // X.509 auth has one step, so we don't collect step-by-step metrics.
                            "summary": {},
                        },
                    },
                },
                1 /* expectedCount */,
                null /* severity */,
                true /* isRelaxed */,
            ),
        `Did not find SASL success log on secondary config server ${conn.host}`,
    );
});

// TODO SERVER-116026: Add failure test case for X.509 authentication logs

st.stop();
