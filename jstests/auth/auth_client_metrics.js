/**
 * Tests that client-side SASL authentication using SCRAM-SHA-256 produces the expected connection
 * health log lines.
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

jsTest.log.info("Check secondary config servers for authentication logs");
st.configRS.getSecondaries().forEach((conn) => {
    // getLog pulls a maximum of 1024 entries, which is sometimes not enough to find intra-cluster
    // auth logs because startup is noisy. Instead, we search the log file directly.
    assert.soon(
        () =>
            checkLog.checkContainsWithAtLeastCountJson(
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
                1 /* expectedCount */,
                null /* severity */,
                true /* isRelaxed */,
            ),
        `Did not find SASL speculative success log for __system@local on secondary config server ${conn.host}`,
    );
    assert.soon(
        () =>
            checkLog.checkContainsWithAtLeastCountJson(
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
                1 /* expectedCount */,
                null /* severity */,
                true /* isRelaxed */,
            ),
        `Did not find SASL success log for __system@local on secondary config server ${conn.host}`,
    );
});

st.stop();
