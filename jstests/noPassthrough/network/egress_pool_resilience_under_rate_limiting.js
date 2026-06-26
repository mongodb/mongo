/**
 * Verifies that mongos's egress connection pool to a shard is not cleared when the shard's
 * connection-establishment rate limiter rejects a new connection attempt.
 *
 * When an established connection already exists in the pool and the shard rate-limits a new
 * establishment, the shard closes the socket. The egress client observes this as a network error
 * during establishment:
 *
 *   non-TLS: graceful FIN → "Connection closed by peer"
 *   TLS    : abrupt close → "Connection closed by peer"
 *
 * The pool must single-drop that one failed attempt and keep its existing connections to the shard,
 * rather than flushing the whole pool. This test verifies that end-to-end.
 *
 * Runs with and without TLS to confirm the behavior holds in both transport modes.
 *
 * gRPC egress does not go through the ASIO connection pool, so the rate-limiter scenario is not
 * applicable there.
 * @tags: [
 *   grpc_incompatible,
 *   requires_sharding,
 *   requires_ssl,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    assertHasConnPoolStats,
    checkHostHasOpenConnections,
} from "jstests/libs/network/conn_pool_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {allowTLS} from "jstests/ssl/libs/ssl_helpers.js";

const kDb = "test";
const kColl = "rate_limiter_pool_resilience";

/**
 * Core test logic.
 *
 * @param {Object} tlsOptions - TLS setParameter/option overrides applied to both shard and mongos.
 *                              Pass {} for non-TLS, or allowTLS for TLS.
 * @param {string} label - Printed in jsTestLog messages.
 */
function runTest(tlsOptions, label) {
    jsTestLog(`=== runTest: ${label} ===`);

    // tlsOptions contains command-line options (tlsMode, tlsCertificateKeyFile, etc.), not server
    // parameters, so spread them at the top level of each node options object rather than inside
    // setParameter.  featureFlagRateLimitIngressConnectionEstablishment is a startup-only server
    // parameter, so it goes inside setParameter on the shard (rsOptions).
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {
            // Raise mongos's egress maxConnecting so several connection attempts to the shard are
            // in flight at once. With the default of 2, the attempts never exceed the rate limiter
            // capacity and no rejection is ever recorded.
            mongosOptions: {
                ...tlsOptions,
                setParameter: {
                    ShardingTaskExecutorPoolMaxConnecting: 20,
                    // Min pool size 0 so mongos doesn't pre-create shard connections: otherwise some
                    // would be established before we tighten the limiter below and never be throttled.
                    // The only established connection should be the one this test creates and pins.
                    ShardingTaskExecutorPoolMinSize: 0,
                },
            },
            rsOptions: {
                ...tlsOptions,
                setParameter: {
                    // Only the feature flag is startup-only (it's a feature flag). The limiter is
                    // left disabled at startup so cluster bring-up and the initial connection are not
                    // throttled; we enable and tighten it entirely at runtime below. The limiter is
                    // fully runtime-configurable -- the enabled flag is read on each new session.
                    featureFlagRateLimitIngressConnectionEstablishment: true,
                },
            },
            configOptions: {...tlsOptions},
        },
    });

    const mongos = st.s0;
    const shardPrimary = st.rs0.getPrimary();
    const shardHost = shardPrimary.host;

    // Create the collection and run an initial find so mongos has at least one established
    // connection to the shard in its pool.
    assert.commandWorked(mongos.getDB(kDb).createCollection(kColl));
    assert.commandWorked(mongos.getDB(kDb).runCommand({find: kColl}));

    // Wait for the pool to reflect the established connection.
    jsTestLog("Waiting for mongos pool to show established connection to shard: " + shardHost);
    assertHasConnPoolStats(
        mongos,
        [shardHost],
        {checkStatsFunc: checkHostHasOpenConnections},
        0 /* checkNum */,
    );

    // Block the shard so the existing connection stays in-use, forcing the pool to attempt
    // new establishments for subsequent requests.
    const blockFP = configureFailPoint(shardPrimary, "waitAfterCommandFinishesExecution", {
        commands: ["find"],
    });

    const blockedThread = new Thread(
        function (host, dbName, collName) {
            const m = new Mongo(host);
            try {
                m.getDB(dbName).runCommand({find: collName});
            } catch (e) {
                // The thread may be interrupted when we disable the failpoint.
            }
        },
        mongos.host,
        kDb,
        kColl,
    );
    blockedThread.start();

    // Wait for the blocking find to be in-flight (connection is in-use on mongos→shard).
    jsTestLog("Waiting for in-use connection on mongos→shard pool");
    assert.soon(
        () => {
            const poolStats = assert.commandWorked(mongos.adminCommand({connPoolStats: 1}));
            const hostStats = poolStats.hosts[shardHost];
            return hostStats && hostStats.inUse >= 1;
        },
        "Timed out waiting for in-use connection in mongos pool",
        30000,
    );

    // Enable and tighten the rate limiter at runtime (it was left disabled at startup) so every new
    // mongos→shard establishment is rejected deterministically, without relying on token-bucket
    // refill timing. Burst capacity must be > 0 (validator), so we set it far below one token's
    // worth: with rate 1/s and burst 0.001s the bucket can never hold a full token, so each new
    // establishment fails the non-blocking token acquire and is rejected immediately; maxQueueDepth 0
    // keeps it on the immediate-reject path. We deliberately do NOT use the hangInRateLimiter
    // failpoint: it parks establishments in a ~1-hour queue wait instead of rejecting them (so no
    // rejection is recorded), and freezing the shard's ingress destabilizes the cluster.
    assert.commandWorked(
        shardPrimary.adminCommand({
            setParameter: 1,
            ingressConnectionEstablishmentRateLimiterEnabled: true,
            ingressConnectionEstablishmentRatePerSec: 1,
            ingressConnectionEstablishmentBurstCapacitySecs: 0.001,
            ingressConnectionEstablishmentMaxQueueDepth: 0,
        }),
    );

    // Capture baseline rejection count.
    const baselineStats = shardPrimary.adminCommand({serverStatus: 1});
    const baselineRejected =
        baselineStats.connections?.establishmentRateLimit?.rejected ?? NumberLong(0);
    jsTestLog("Baseline rejected: " + tojson(baselineRejected));

    // Send several parallel finds through mongos. The one existing connection is pinned in-use by the
    // failpoint above and every new establishment is rejected, so none of these can get a connection:
    // each must fail by timing out while waiting for one. Each thread returns its command response so
    // the parent can assert that failure mode below.
    const kNumParallelFinds = 10;
    const threads = [];
    for (let i = 0; i < kNumParallelFinds; i++) {
        const t = new Thread(
            function (host, dbName, collName) {
                const m = new Mongo(host);
                // A command-level timeout comes back as {ok: 0, code: ...} (no throw); a
                // transport-level failure throws -- normalize both to a response object.
                try {
                    return m.getDB(dbName).runCommand({find: collName, maxTimeMS: 10000});
                } catch (e) {
                    return {ok: 0, code: e.code, errmsg: e.message};
                }
            },
            mongos.host,
            kDb,
            kColl,
        );
        t.start();
        threads.push(t);
    }

    // Wait for shard to report at least one rejection from the rate limiter.
    jsTestLog("Waiting for rate limiter to reject at least one connection");
    assert.soon(
        () => {
            const ss = shardPrimary.adminCommand({serverStatus: 1});
            const rejected = ss.connections?.establishmentRateLimit?.rejected ?? NumberLong(0);
            return rejected > baselineRejected;
        },
        "Timed out waiting for rate-limiter rejections on shard",
        30000,
    );

    // KEY ASSERTION: mongos pool must still have the established connection (pool not cleared).
    jsTestLog(
        "Asserting mongos pool still has established connection after rate-limiter rejections",
    );
    const poolStats = assert.commandWorked(mongos.adminCommand({connPoolStats: 1}));
    jsTestLog("connPoolStats.hosts[" + shardHost + "]: " + tojson(poolStats.hosts[shardHost]));
    const hostStats = poolStats.hosts[shardHost];
    assert(hostStats, "No pool entry for shard host " + shardHost);
    assert.gt(
        hostStats.inUse + hostStats.available + hostStats.leased + hostStats.refreshing,
        0,
        "mongos pool was unexpectedly cleared for shard " +
            shardHost +
            " after rate-limiter rejections. Pool entry: " +
            tojson(hostStats),
    );

    // Every parallel find must have failed by timing out waiting for a connection -- NOT with a
    // propagated network error, which would mean the pool returned the rate-limiter rejection (or
    // cleared) instead of absorbing it. Join them while the limit is still in effect and the lone
    // connection is still pinned, and assert that failure mode for all of them.
    for (const t of threads) {
        t.join();
        assert.commandFailedWithCode(
            t.returnData(),
            [ErrorCodes.MaxTimeMSExpired, ErrorCodes.PooledConnectionAcquisitionExceededTimeLimit],
            "find should time out waiting for a connection, not receive a propagated pool error",
        );
    }

    // Clean up: disable failpoint and rate limiter.
    blockFP.off();
    blockedThread.join();

    // Disable rate limiter so the cluster can shut down cleanly.
    assert.commandWorked(
        shardPrimary.adminCommand({
            setParameter: 1,
            ingressConnectionEstablishmentRateLimiterEnabled: false,
        }),
    );

    // After disabling the rate limiter, new ops should succeed (pool is still healthy).
    jsTestLog("Verifying ops succeed after rate limiter is disabled");
    assert.commandWorked(mongos.getDB(kDb).runCommand({find: kColl}));

    st.stop();
    jsTestLog(`=== runTest: ${label} PASSED ===`);
}

// Run without TLS.
runTest({}, "non-TLS");

// Run with TLS (allowTLS on both shard and mongos so mongos→shard connection negotiates TLS).
// @tags: [requires_ssl]
runTest(allowTLS, "TLS");
