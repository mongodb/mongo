/**
 * Tests that client-side SASL authentication using X.509 produces the expected connection health
 * log lines and increments the auth counter as expected.
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
    const external = conn.getDB("$external");
    assert.eq(
        1,
        external.auth({
            mechanism: "MONGODB-X509",
        }),
        `X.509 auth failed on secondary ${conn.host} before running serverStatus`,
    );
    const stats = assert.commandWorked(external.runCommand({serverStatus: 1})).security.authentication;
    jsTest.log.info("Authn stats: " + tojson(stats));
    assert.gt(stats.totalEgressAuthenticationTimeMicros, 0);
    const mechStats = stats.mechanisms;

    let egressAuthSuccesses;
    // We should see egress MONGODB-X509 auths and speculative auths, and no other egress auths
    for (const [mech, stats] of Object.entries(mechStats)) {
        if (mech === "MONGODB-X509") {
            egressAuthSuccesses = stats.egress.authenticate.successful;
            assert.gte(
                egressAuthSuccesses,
                1,
                "Expected at least one egress MONGODB-X509 authenticate success on " + conn.host,
            );
            assert.eq(
                egressAuthSuccesses,
                stats.egress.authenticate.total,
                "MONGODB-X509 egress authenticate successful count should equal total on " + conn.host,
            );
            assert.eq(
                stats.egress.speculativeAuthenticate.total,
                0,
                "Expected no MONGODB-X509 speculativeAuthenticate attempts on " + conn.host,
            );
        } else {
            assert.eq(
                stats.egress.authenticate.total,
                0,
                "Mechanism " + mech + " should have no egress authenticate attempts on " + conn.host,
            );
            assert.eq(
                stats.egress.speculativeAuthenticate.total,
                0,
                "Mechanism " + mech + " should have no egress speculativeAuthenticate attempts on " + conn.host,
            );
            if (stats.hasOwnProperty("ingress")) {
                assert.eq(
                    stats.ingress.authenticate.total,
                    0,
                    "Mechanism " + mech + " should have no ingress authenticate attempts on " + conn.host,
                );
                assert.eq(
                    stats.ingress.speculativeAuthenticate.total,
                    0,
                    "Mechanism " + mech + " should have no ingress speculativeAuthenticate attempts on " + conn.host,
                );
                assert.eq(
                    stats.ingress.clusterAuthenticate.total,
                    0,
                    "Mechanism " + mech + " should have no ingress clusterAuthenticate attempts on " + conn.host,
                );
            }
        }
    }
    // getLog pulls a maximum of 1024 entries, which is sometimes not enough to find intra-cluster
    // auth logs because startup is noisy. Instead, we search the log file directly.
    const successMessages = checkLog.getFilteredLogMessages(
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
        null /* severity */,
        true /* isRelaxed */,
    );
    assert.lte(
        Math.abs(successMessages.length - egressAuthSuccesses),
        2,
        "X.509 success log message count should be roughly egress auth success count on " + conn.host,
    );
});

// TODO SERVER-116026: Add failure test case for X.509 authentication logs

st.stop();
