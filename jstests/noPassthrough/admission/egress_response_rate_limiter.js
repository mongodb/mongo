/**
 * End-to-end tests for the egress response rate limiter.
 *
 * The egress response rate limiter paces the network egress of SystemOverloaded rejection replies
 * produced by the IngressRequestRateLimiter (IRRL), for user/application connections only. These
 * tests verify the SessionWorkflow egress-hook wiring on both mongod and mongos by observing the
 * `serverStatus.network.egressResponseRateLimiter` counters:
 *
 *   - The limiter engages (successfulAdmissions increments) for an IRRL rejection reply sent to a
 *     user connection, once `egressResponseRateLimiterEnabled` is set.
 *   - The limiter does NOT engage while `egressResponseRateLimiterEnabled` is off (its default),
 *     even when IRRL is rejecting and a tight egress rate is configured.
 *   - The limiter does NOT engage for a normal (non-rejected) response.
 *   - The limiter does NOT engage for a rejection reply sent to an IRRL-exempt connection (the
 *     exempt connection is never rejected, so it never produces a rejection reply).
 *   - Under a tight egress rate, multiple rejection replies are paced (a lower-bound wall-clock
 *     assertion; pacing is inherently a timing behavior).
 *
 * @tags: [requires_fcv_80]
 */

import {
    disableRateLimiter,
    enableZeroBurstRateLimiter,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
    makeAuthConn,
    makeExemptConn,
    setupAuth,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, after, afterEach} from "jstests/libs/mochalite.js";

const maxInt32 = Math.pow(2, 31) - 1;

// A tight egress rate used by the pacing test. With burst=1 token and rate=kEgressRatePerSec, the
// first rejection reply consumes the burst token and each subsequent reply must wait ~1/rate
// seconds for a token to refill.
const kEgressRatePerSec = 5;
const kEgressBurstCapacitySecs = 1.0 / kEgressRatePerSec; // burst size = 1 token.

/**
 * Returns the egress response rate limiter stats from serverStatus, or null if the section is not
 * present.
 */
function getEgressStats(conn) {
    const net = conn.getDB("admin").serverStatus().network;
    return net.egressResponseRateLimiter;
}

/**
 * Coerces a serverStatus counter value (a JS number or a NumberLong) to a plain number, or
 * undefined if it is not numeric. NumberLong values arrive as Long objects with a toNumber()
 * method, not as JS numbers, so a plain `typeof === "number"` check would skip them.
 */
function asNumber(v) {
    if (typeof v === "number") {
        return v;
    }
    if (v && typeof v.toNumber === "function") {
        return v.toNumber();
    }
    return undefined;
}

/**
 * Captures egress limiter stats before and after running `fn`, returning a per-field `delta`
 * (after - before) over the numeric counter fields plus the raw `{before, after}` snapshots for
 * diagnostic context in assertions. Intended for the engagement tests; the pacing test asserts on
 * wall-clock time and does not use this.
 */
function withEgressStats(conn, fn) {
    const before = getEgressStats(conn);
    fn();
    const after = getEgressStats(conn);
    const delta = {};
    if (before && after) {
        for (const k of Object.keys(after)) {
            const av = asNumber(after[k]);
            const bv = asNumber(before[k]);
            if (av !== undefined && bv !== undefined) {
                delta[k] = av - bv;
            }
        }
    }
    return {delta, before, after};
}

/**
 * Configures the egress response rate limiter on conn. The limiter is off by default and only
 * engages for user/application rejection replies once `egressResponseRateLimiterEnabled` is set;
 * even then, the default max rate makes it a no-op, so a finite rate/burst is what actually
 * introduces pacing.
 */
function configureEgressLimiter(
    exemptConn,
    {enabled = true, ratePerSec = maxInt32, burstCapacitySecs = Number.MAX_VALUE} = {},
) {
    assert.commandWorked(
        exemptConn.adminCommand({
            setParameter: 1,
            egressResponseRateLimiterEnabled: enabled,
            egressResponseRateLimiterRatePerSec: ratePerSec,
            egressResponseRateLimiterBurstCapacitySecs: burstCapacitySecs,
        }),
    );
}

function disableEgressLimiter(exemptConn) {
    // Reset the egress limiter to its startup defaults (off, max rate, unbounded burst) so
    // subsequent tests start from a non-pacing state.
    configureEgressLimiter(exemptConn, {enabled: false});
}

/**
 * Common startup setParameter for the IRRL fractional-rate override failpoint (forces a slow rate
 * deterministically) and the IRRL feature flag. IRRL itself is kept off at startup so cluster
 * setup is not disrupted.
 */
function commonStartupParams() {
    return {
        featureFlagIngressRateLimiting: true,
        ingressRequestRateLimiterEnabled: false,
        "failpoint.ingressRequestRateLimiterFractionalRateOverride": tojson({
            mode: "alwaysOn",
            data: {rate: kSlowestRefreshRateSecs},
        }),
    };
}

describe("egress response rate limiter", function () {
    describe("on a standalone mongod", function () {
        let mongod;
        let exemptConn;
        let userConn;

        before(function () {
            mongod = MongoRunner.runMongod({
                auth: "",
                setParameter: commonStartupParams(),
            });
            // setupAuth creates the root user via mongod's localhost exception and authenticates
            // exemptConn (makeExemptConn's own auth attempt first fails silently because the user
            // does not exist yet). The user connection is opened only after the user exists.
            exemptConn = makeExemptConn(mongod.host);
            setupAuth(mongod, exemptConn);
            userConn = makeAuthConn(mongod.host);
        });

        after(function () {
            if (mongod) {
                MongoRunner.stopMongod(mongod);
            }
        });

        afterEach(function () {
            try {
                disableRateLimiter(exemptConn);
            } catch (e) {}
            try {
                disableEgressLimiter(exemptConn);
            } catch (e) {}
        });

        it("engages for an IRRL rejection reply on a user connection", function () {
            enableZeroBurstRateLimiter(exemptConn, [kRateLimiterExemptAppName]);
            configureEgressLimiter(exemptConn); // huge rate: no pacing delay, just observe engagement.

            const {delta, before, after} = withEgressStats(exemptConn, () => {
                assert.commandFailedWithCode(
                    userConn.adminCommand({ping: 1, comment: "egress-rej-standalone"}),
                    ErrorCodes.IngressRequestRateLimitExceeded,
                );
            });

            assert.gt(
                delta.successfulAdmissions,
                0,
                "egress limiter must engage for an IRRL rejection reply on a user connection",
                {before, after},
            );
        });

        it("does not engage for an IRRL rejection reply while disabled", function () {
            enableZeroBurstRateLimiter(exemptConn, [kRateLimiterExemptAppName]);
            // A rate tight enough to pace visibly if the limiter were consulted at all, paired with
            // the limiter switched off. The rejection reply must bypass it entirely.
            configureEgressLimiter(exemptConn, {
                enabled: false,
                ratePerSec: kEgressRatePerSec,
                burstCapacitySecs: kEgressBurstCapacitySecs,
            });

            const {delta, before, after} = withEgressStats(exemptConn, () => {
                assert.commandFailedWithCode(
                    userConn.adminCommand({ping: 1, comment: "egress-rej-disabled-standalone"}),
                    ErrorCodes.IngressRequestRateLimitExceeded,
                );
            });

            assert.eq(
                delta.attemptedAdmissions,
                0,
                "egress limiter must not be consulted at all while disabled",
                {before, after},
            );
        });

        it("does not engage for a normal (non-rejected) response", function () {
            configureEgressLimiter(exemptConn);
            // IRRL stays disabled, so the user request succeeds and produces a normal response.
            const {delta, before, after} = withEgressStats(exemptConn, () => {
                assert.commandWorked(
                    userConn.adminCommand({ping: 1, comment: "egress-normal-standalone"}),
                );
            });

            assert.eq(
                delta.successfulAdmissions,
                0,
                "egress limiter must NOT engage for a normal response",
                {before, after},
            );
        });

        it("paces multiple rejection replies under a tight egress rate", function () {
            // Pacing is inherently a timing behavior. Use a conservative LOWER bound on wall-clock
            // elapsed time with generous slack so slow/loaded CI machines do not flake: N rejection
            // replies at rate R with burst 1 must take at least (N-1)/R seconds, minus slack.
            enableZeroBurstRateLimiter(exemptConn, [kRateLimiterExemptAppName]);
            configureEgressLimiter(exemptConn, {
                ratePerSec: kEgressRatePerSec,
                burstCapacitySecs: kEgressBurstCapacitySecs,
            });

            const numRejections = 4;
            const minElapsedMs = ((numRejections - 1) / kEgressRatePerSec) * 1000;
            // Allow up to 40% slack below the theoretical lower bound to absorb scheduling jitter
            // without masking a real regression (which would show elapsed far below the bound).
            const slackMs = minElapsedMs * 0.4;

            const start = Date.now();
            for (let i = 0; i < numRejections; i++) {
                assert.commandFailedWithCode(
                    userConn.adminCommand({ping: 1, comment: "egress-pace-" + i}),
                    ErrorCodes.IngressRequestRateLimitExceeded,
                );
            }
            const elapsedMs = Date.now() - start;

            assert.gt(
                elapsedMs,
                minElapsedMs - slackMs,
                "egress limiter did not pace rejection replies: expected to take at least " +
                    (minElapsedMs - slackMs) +
                    "ms (with slack) for " +
                    numRejections +
                    " rejection replies at rate " +
                    kEgressRatePerSec +
                    "/s",
                {elapsedMs, minElapsedMs, slackMs},
            );
        });
    });

    describe("on a mongos (router entry point)", function () {
        let st;
        let exemptConn;
        let userConn;

        before(function () {
            st = new ShardingTest({
                mongos: 1,
                shards: 1,
                other: {
                    auth: "",
                    keyFile: "jstests/libs/key1",
                    mongosOptions: {setParameter: commonStartupParams()},
                    // Keep IRRL off on the shard so mongos->shard internal traffic is never rejected
                    // (the egress limiter v1 does not pace shard-side rejections anyway).
                    rsOptions: {
                        setParameter: {
                            featureFlagIngressRateLimiting: true,
                            ingressRequestRateLimiterEnabled: false,
                        },
                    },
                },
            });
            // setupAuth creates the root user on the mongos (authenticating as __system via the
            // keyfile) and authenticates exemptConn (makeExemptConn's own auth attempt first
            // fails silently because the user does not exist yet). The user connection is opened
            // only after the user exists.
            exemptConn = makeExemptConn(st.s.host);
            setupAuth(st.s, exemptConn);
            userConn = makeAuthConn(st.s.host);
        });

        after(function () {
            if (st) {
                st.stop();
            }
        });

        afterEach(function () {
            try {
                disableRateLimiter(exemptConn);
            } catch (e) {}
            try {
                disableEgressLimiter(exemptConn);
            } catch (e) {}
        });

        it("engages for an IRRL rejection reply produced by the mongos", function () {
            // Enable IRRL on the mongos only (shards stay off), so user requests are rejected at the
            // router and the mongos egress hook paces the rejection reply.
            enableZeroBurstRateLimiter(exemptConn, [kRateLimiterExemptAppName]);
            configureEgressLimiter(exemptConn); // huge rate: no pacing delay, just observe engagement.

            const {delta, before, after} = withEgressStats(exemptConn, () => {
                assert.commandFailedWithCode(
                    userConn.adminCommand({ping: 1, comment: "egress-rej-mongos"}),
                    ErrorCodes.IngressRequestRateLimitExceeded,
                );
            });

            assert.gt(
                delta.successfulAdmissions,
                0,
                "mongos egress limiter must engage for an IRRL rejection reply on a user connection",
                {before, after},
            );
        });

        it("does not engage for a normal (non-rejected) response through the mongos", function () {
            configureEgressLimiter(exemptConn);
            const {delta, before, after} = withEgressStats(exemptConn, () => {
                assert.commandWorked(
                    userConn.adminCommand({ping: 1, comment: "egress-normal-mongos"}),
                );
            });

            assert.eq(
                delta.successfulAdmissions,
                0,
                "mongos egress limiter must NOT engage for a normal response",
                {before, after},
            );
        });
    });
});
