/**
 * Tests IRRL error propagation for scatter-gather queries (find/aggregation that fan out to
 * multiple shards). Scatter-gather operations route through NetworkInterfaceTL-TaskExecutorPool-{i}
 * (non-exempt), so shard rejections propagate back through mongos.
 *
 * A 2x2 matrix of scenarios:
 *   - One shard overloaded vs. both shards overloaded
 *   - Budget exhaustion (0 retries) vs. retry cap exhausted (K retries, K+1 failures)
 *
 * @tags: [requires_fcv_80]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertMongosIRRLCommandFailure,
    assertShardingStatisticsDiffEq,
    authenticateConnection,
    disableFailCommandOnShards,
    enableFailCommandOnShards,
    getShardStats,
    kKeyFile,
    kRateLimiterExemptAppName,
    setParameter,
    setupAuth,
    shardingStatisticsDifference,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kRetryAttempts = 3;
const kTestNamespace = "test.coll";

describe("scatter-gather queries with ingress rate limiter", function () {
    let st;
    let adminMongosConn;
    let rateLimitedConn;

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                auth: "",
                keyFile: kKeyFile,
                rsOptions: {
                    setParameter: {ingressRequestRateLimiterEnabled: 0},
                },
            },
        });

        adminMongosConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        setupAuth(st.s, adminMongosConn);

        assert.commandWorked(
            adminMongosConn.adminCommand({
                enableSharding: "test",
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            adminMongosConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}),
        );
        assert.commandWorked(adminMongosConn.adminCommand({split: "test.coll", middle: {_id: 0}}));
        assert.commandWorked(
            adminMongosConn.adminCommand({
                moveChunk: "test.coll",
                find: {_id: 1},
                to: st.shard1.shardName,
            }),
        );
        assert.commandWorked(adminMongosConn.getDB("test").coll.insertMany([{_id: -1}, {_id: 1}]));

        rateLimitedConn = new Mongo(st.s.host);
        authenticateConnection(rateLimitedConn);
    });

    after(function () {
        st.stop();
    });

    describe("one shard overloaded", function () {
        it("budget exhaustion: scatter-gather find fails immediately with retries disabled", function () {
            const origParams = setParameter(adminMongosConn, {defaultClientMaxRetryAttempts: 0});

            const shard0Name = st.shard0.shardName;
            const statsBefore = getShardStats(adminMongosConn, shard0Name);
            const shard0Primary = st.rs0.getPrimary().host;
            enableFailCommandOnShards(shard0Primary, {times: 1}, ["find"], kTestNamespace);

            try {
                const result = rateLimitedConn.getDB("test").runCommand({find: "coll"});
                assertMongosIRRLCommandFailure(
                    result,
                    "scatter-gather find must fail with shard rate limit rejection",
                );

                const diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard0Name),
                    statsBefore,
                );
                assertShardingStatisticsDiffEq(diff, {
                    numOverloadErrorsReceived: 1,
                    numRetriesDueToOverloadAttempted: 0,
                    numOperationsRetriedAtLeastOnceDueToOverload: 0,
                    numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                });
            } finally {
                disableFailCommandOnShards(shard0Primary);
                setParameter(adminMongosConn, origParams);
            }
        });

        it("retry cap: scatter-gather find fails after exhausting all retries", function () {
            const origParams = setParameter(adminMongosConn, {
                defaultClientMaxRetryAttempts: kRetryAttempts,
                defaultClientBaseBackoffMillis: 0,
                defaultClientMaxBackoffMillis: 0,
            });

            const shard0Name = st.shard0.shardName;
            const statsBefore = getShardStats(adminMongosConn, shard0Name);
            const shard0Primary = st.rs0.getPrimary().host;
            enableFailCommandOnShards(
                shard0Primary,
                {times: kRetryAttempts + 1},
                ["find"],
                kTestNamespace,
            );

            try {
                const result = rateLimitedConn.getDB("test").runCommand({find: "coll"});
                assertMongosIRRLCommandFailure(
                    result,
                    "scatter-gather find must fail after exhausting all retries",
                );

                const diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard0Name),
                    statsBefore,
                );
                assertShardingStatisticsDiffEq(diff, {
                    numOverloadErrorsReceived: kRetryAttempts + 1,
                    numRetriesDueToOverloadAttempted: kRetryAttempts,
                    numOperationsRetriedAtLeastOnceDueToOverload: 1,
                    numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded: 0,
                });
            } finally {
                disableFailCommandOnShards(shard0Primary);
                setParameter(adminMongosConn, origParams);
            }
        });
    });

    describe("both shards overloaded", function () {
        it("budget exhaustion: scatter-gather find fails immediately with retries disabled", function () {
            const origParams = setParameter(adminMongosConn, {defaultClientMaxRetryAttempts: 0});

            const shard0Name = st.shard0.shardName;
            const shard1Name = st.shard1.shardName;
            const shard0StatsBefore = getShardStats(adminMongosConn, shard0Name);
            const shard1StatsBefore = getShardStats(adminMongosConn, shard1Name);
            const shard0Primary = st.rs0.getPrimary().host;
            const shard1Primary = st.rs1.getPrimary().host;
            enableFailCommandOnShards(shard0Primary, {times: 1}, ["find"], kTestNamespace);
            enableFailCommandOnShards(shard1Primary, {times: 1}, ["find"], kTestNamespace);

            try {
                const result = rateLimitedConn.getDB("test").runCommand({find: "coll"});
                assertMongosIRRLCommandFailure(
                    result,
                    "scatter-gather find must fail when all shards reject",
                );

                const shard0Diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard0Name),
                    shard0StatsBefore,
                );
                const shard1Diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard1Name),
                    shard1StatsBefore,
                );
                assertShardingStatisticsDiffEq(shard0Diff, {
                    numOverloadErrorsReceived: 1,
                    numRetriesDueToOverloadAttempted: 0,
                });
                assertShardingStatisticsDiffEq(shard1Diff, {
                    numOverloadErrorsReceived: 1,
                    numRetriesDueToOverloadAttempted: 0,
                });
            } finally {
                disableFailCommandOnShards(shard0Primary);
                disableFailCommandOnShards(shard1Primary);
                setParameter(adminMongosConn, origParams);
            }
        });

        it("retry cap: scatter-gather find fails after exhausting all retries on all shards", function () {
            const origParams = setParameter(adminMongosConn, {
                defaultClientMaxRetryAttempts: kRetryAttempts,
                defaultClientBaseBackoffMillis: 0,
                defaultClientMaxBackoffMillis: 0,
            });

            const shard0Name = st.shard0.shardName;
            const shard1Name = st.shard1.shardName;
            const shard0StatsBefore = getShardStats(adminMongosConn, shard0Name);
            const shard1StatsBefore = getShardStats(adminMongosConn, shard1Name);
            const shard0Primary = st.rs0.getPrimary().host;
            const shard1Primary = st.rs1.getPrimary().host;
            enableFailCommandOnShards(
                shard0Primary,
                {times: kRetryAttempts + 1},
                ["find"],
                kTestNamespace,
            );
            enableFailCommandOnShards(
                shard1Primary,
                {times: kRetryAttempts + 1},
                ["find"],
                kTestNamespace,
            );

            try {
                const result = rateLimitedConn.getDB("test").runCommand({find: "coll"});
                assertMongosIRRLCommandFailure(
                    result,
                    "scatter-gather find must fail after exhausting all retries on all shards",
                );

                const shard0Diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard0Name),
                    shard0StatsBefore,
                );
                const shard1Diff = shardingStatisticsDifference(
                    getShardStats(adminMongosConn, shard1Name),
                    shard1StatsBefore,
                );
                // Both shards race through retries concurrently; whichever exhausts its budget first
                // calls stopRetrying(), cutting off the other mid-sequence — so only one shard is
                // guaranteed to hit the full cap of kRetryAttempts + 1 errors.
                assert.between(1, shard0Diff.numOverloadErrorsReceived, kRetryAttempts + 1);
                assert.between(1, shard1Diff.numOverloadErrorsReceived, kRetryAttempts + 1);
                assert.eq(
                    Math.max(
                        shard0Diff.numOverloadErrorsReceived,
                        shard1Diff.numOverloadErrorsReceived,
                    ),
                    kRetryAttempts + 1,
                );
                assert.eq(
                    shard0Diff.numRetriesDueToOverloadAttempted,
                    Math.min(shard0Diff.numOverloadErrorsReceived, kRetryAttempts),
                );
                assert.eq(
                    shard1Diff.numRetriesDueToOverloadAttempted,
                    Math.min(shard1Diff.numOverloadErrorsReceived, kRetryAttempts),
                );
            } finally {
                disableFailCommandOnShards(shard0Primary);
                disableFailCommandOnShards(shard1Primary);
                setParameter(adminMongosConn, origParams);
            }
        });
    });
});
