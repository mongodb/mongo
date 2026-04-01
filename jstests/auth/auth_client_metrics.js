/**
 * Tests that client-side SASL authentication using SCRAM-SHA-256 produces the expected connection
 * health log lines and increments the auth counter as expected.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const kClientSaslSuccessLogId = 10748701;
const kClientSaslSpeculativeSuccessLogId = 10748710;

// Startup of an auth-enabled sharded cluster will use SCRAM-SHA-256 for intra-cluster auth, we will
// validate corresponding logs
const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    keyFile: "jstests/libs/key1",
    other: {
        configOptions: {useLogFiles: true},
    },
});

// Make admin user so we can fetch server status
const admin = st.s.getDB("admin");
admin.createUser({
    user: "root",
    pwd: "pass",
    roles: ["root"],
});
assert.eq(1, admin.auth("root", "pass"), "Authentication for root user failed");

jsTest.log.info("Check secondary config servers for authentication logs");
st.configRS.getSecondaries().forEach((conn) => {
    // Get per-mech auth counter stats from serverStatus
    const admin = conn.getDB("admin");
    assert.soon(() => admin.auth("root", "pass"), "Authentication for root user failed on " + conn.host);
    const stats = assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication;
    jsTest.log.info("Authn stats: " + tojson(stats));
    assert.gt(stats.totalEgressAuthenticationTimeMicros, 0);

    const mechStats = stats.mechanisms;
    let egressAuthSuccesses, egressSpecAuthSuccesses;
    // We should see egress SCRAM-SHA-256 auths and speculative auths, and no other egress auths
    for (const [mech, stats] of Object.entries(mechStats)) {
        if (mech === "SCRAM-SHA-256") {
            egressAuthSuccesses = stats.egress.authenticate.successful;
            egressSpecAuthSuccesses = stats.egress.speculativeAuthenticate.successful;
            assert.gte(
                egressAuthSuccesses,
                1,
                "Expected at least one egress SCRAM-SHA-256 authenticate success on " + conn.host,
            );
            assert.gte(
                egressSpecAuthSuccesses,
                1,
                "Expected at least one egress SCRAM-SHA-256 speculativeAuthenticate success on " + conn.host,
            );
            assert.eq(
                egressAuthSuccesses,
                stats.egress.authenticate.total,
                "SCRAM-SHA-256 egress authenticate successful count should equal total on " + conn.host,
            );
            assert.eq(
                egressSpecAuthSuccesses,
                stats.egress.speculativeAuthenticate.total,
                "SCRAM-SHA-256 egress speculativeAuthenticate successful count should equal total on " + conn.host,
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
    // Check that egress speculative/normal auths are all logged, and that the counts match those
    // returned by serverStatus.
    // getLog pulls a maximum of 1024 entries, which is sometimes not enough to find intra-cluster
    // auth logs because startup is noisy. Instead, we search the log file directly.
    const specSuccessMessages = checkLog.getFilteredLogMessages(
        conn.fullOptions.logFile,
        kClientSaslSpeculativeSuccessLogId,
        {
            "username": "__system",
            "targetDatabase": "local",
            "mechanism": "SCRAM-SHA-256",
            "result": 0,
            "metrics": {
                "conversation_duration": {
                    // Speculative auth doesn't record steps
                    "summary": {},
                },
            },
        },
        null /* severity */,
        true /* isRelaxed */,
    );
    assert.gte(
        specSuccessMessages.length,
        1,
        `Did not find SASL speculative success log for __system@local on secondary config server ${conn.host}`,
    );
    const successMessages = checkLog.getFilteredLogMessages(
        conn.fullOptions.logFile,
        kClientSaslSuccessLogId,
        {
            "username": "__system",
            "targetDatabase": "local",
            "mechanism": "SCRAM-SHA-256",
            "result": 0,
            "metrics": {
                "conversation_duration": {
                    "summary": {
                        // Validate that SCRAM-SHA-256 has 3 steps
                        "0": {
                            "step": 1,
                            "step_total": 3,
                        },
                        "1": {
                            "step": 2,
                            "step_total": 3,
                        },
                        "2": {
                            "step": 3,
                            "step_total": 3,
                        },
                    },
                },
            },
        },
        null /* severity */,
        true /* isRelaxed */,
    );
    assert.gte(
        successMessages.length,
        1,
        `Did not find SASL success log for __system@local on secondary config server ${conn.host}`,
    );

    assert.lte(
        Math.abs(egressAuthSuccesses - (specSuccessMessages.length + successMessages.length)),
        2,
        "Egress auth success count should be roughly sum of speculative and normal auth log messages on " + conn.host,
    );
    assert.lte(
        Math.abs(egressSpecAuthSuccesses - specSuccessMessages.length),
        2,
        "Egress speculative auth success count should be roughly speculative success log message count on " + conn.host,
    );
});

st.stop();
