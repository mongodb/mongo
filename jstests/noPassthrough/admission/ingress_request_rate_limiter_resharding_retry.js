/**
 * Tests resharding correctness when shard nodes are under the ingress request rate limiter
 * (IRRL). Resharding coordinator connections (NetworkInterfaceTL-Resharding*) and inter-shard
 * commands routed through ShardRemote (NetworkInterfaceTL-Sharding-Fixed) are exempt, so
 * coordinator scatter-gather, critical-section, abort, index builds, and oplog fetch are
 * unaffected by the rate limiter. Document cloning uses NetworkInterfaceTL-TaskExecutorPool
 * (not exempt), so recipient→donor clone connections receive IngressRequestRateLimitExceeded
 * (462) when the bucket is exhausted and must retry.
 *
 * The IRRL is enabled with burst capacity zero and a near-zero refill rate so that every
 * non-exempt connection is immediately rejected. Resharding coordinator pause failpoints gate
 * the IRRL enable/disable at precise phase boundaries, avoiding timing races.
 *
 * Five scenarios, ordered by resharding phase:
 *
 *   1. Transient IngressRequestRateLimitExceeded during document cloning:
 *      Coordinator paused before initialization; IRRL engaged on shard nodes. The
 *      initialization scatter-gather uses NetworkInterfaceTL-ReshardingCoordinatorService-
 *      Network (exempt) and succeeds. Subsequent document cloning (TaskExecutorPool, not
 *      exempt) receives IngressRequestRateLimitExceeded and retries until IRRL clears.
 *      Verified via log 5269300 on the recipient.
 *
 *   2. IRRL active during index builds does not disrupt resharding:
 *      reshardingPauseRecipientBeforeBuildingIndex gates IRRL enable to the precise moment
 *      cloning ends and index builds begin. getCollectionOptions and index build commands route
 *      through ShardRemote::_runCommand → getFixedExecutor() → NetworkInterfaceTL-Sharding-Fixed,
 *      which is in the exemption list. IRRL therefore has no effect on this phase. The test
 *      confirms resharding commits with the correct full index set despite IRRL being active.
 *
 *   3. IRRL active during oplog application does not disrupt resharding:
 *      reshardingPauseRecipientBeforeOplogApplication gates the applying phase. The oplog
 *      fetcher calls shard->runAggregation, which routes through ShardRemote::_runAggregation
 *      → getFixedExecutor() → NetworkInterfaceTL-Sharding-Fixed (exempt). IRRL therefore has
 *      no effect on oplog fetch connections. The test confirms resharding commits normally
 *      despite IRRL being active on shard nodes during the applying phase.
 *
 *   4. Critical section commits despite IRRL being active:
 *      IRRL is engaged just before the critical section. All coordinator operations in the
 *      critical section use NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (covered
 *      by the "NetworkInterfaceTL-Resharding" prefix exemption), so they succeed regardless
 *      of the rate limiter. Resharding commits correctly, demonstrating that the exemption
 *      list is correctly designed for production robustness.
 *
 *   5. Abort via abortReshardCollection while IRRL is active:
 *      IRRL is engaged while resharding is mid-cloning. abortReshardCollection is then issued
 *      via an exempt connection. The abort scatter-gather uses
 *      NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (exempt) so it completes
 *      without needing the IRRL cleared. The original collection is intact and no orphaned
 *      system.resharding.* collections remain.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    kExemptions,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kKeyFile = "jstests/libs/key1";
const kUser = "admin";
const kPass = "pwd";
const kNumDocs = 10;

/**
 * This enables an overloaded IRRL on conn with near-zero burst capacity and a near-zero refill rate.
 * Every non-exempt connection is immediately rejected with IngressRequestRateLimitExceeded.
 * Coordinator/Sharding-Fixed internal clients are exempt via kExemptions, while cloning traffic
 * via NetworkInterfaceTL-TaskExecutorPool is not exempt and is expected to hit this error.
 */
function enableZeroBurstRateLimiter(conn) {
    authutil.asCluster(conn, kKeyFile, () => {
        configureFailPoint(conn, "ingressRequestRateLimiterFractionalRateOverride", {
            rate: kSlowestRefreshRateSecs,
        });
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: 1,
                // Effective burstSize = rateOverride × burstCapacitySecs = 5e-6 × 5e-6 = 2.5e-11 tokens.
                // A single request costs 1 token, so the bucket can never accumulate a usable token and every
                // non-exempt connection is immediately rejected. The rate override keeps refill at 5e-6 tokens/s,
                // so the bucket never accumulates a usable token.
                ingressRequestAdmissionBurstCapacitySecs: kSlowestRefreshRateSecs,
                ingressRequestRateLimiterApplicationExemptions: {appNames: kExemptions},
                ingressRequestRateLimiterEnabled: 1,
            }),
        );
    });
}

/**
 * Disables the IRRL on the node at host. Opens a fresh exempt connection so it succeeds
 * even while the rate limiter is active (exempt connections bypass the token bucket).
 */
function disableRateLimiter(host) {
    const conn = new Mongo(`mongodb://${host}/?appName=${kRateLimiterExemptAppName}`);
    authutil.asCluster(conn, kKeyFile, () => {
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: Math.pow(2, 31) - 1,
                ingressRequestAdmissionBurstCapacitySecs: Number.MAX_VALUE,
                ingressRequestRateLimiterEnabled: 0,
            }),
        );
    });
}

/**
 * Opens an exempt connection to host authenticated as __system via keyfile SCRAM-SHA-256.
 * Used for getLog polling and other privileged operations on direct shard connections.
 * The exempt appName bypasses the rate limiter; __system auth is necessary because the
 * admin/pwd user is stored on the config server and cannot authenticate on direct shard
 * connections in a keyfile-auth cluster.
 */
function makeKeyfileExemptConn(host) {
    const conn = new Mongo(`mongodb://${host}/?appName=${kRateLimiterExemptAppName}`);
    authutil.assertAuthenticate(conn, "admin", {
        user: "__system",
        mechanism: "SCRAM-SHA-256",
        pwd: cat(kKeyFile).replace(/[\011-\015\040]/g, ""),
    });
    return conn;
}

function runResharding(mongosHost, appName, user, pass, recipientShardName) {
    const conn = new Mongo(`mongodb://${mongosHost}/?appName=${appName}`);
    conn.getDB("admin").auth(user, pass);
    return conn.adminCommand({
        reshardCollection: "test.coll",
        key: {newKey: 1},
        _presetReshardedChunks: [
            {min: {newKey: MinKey}, max: {newKey: MaxKey}, recipientShardId: recipientShardName},
        ],
    });
}

describe("resharding handles IngressRequestRateLimitExceeded from shard nodes", function () {
    let st;
    let exemptConn;
    let configPrimary;
    let shardPrimaries;
    let recipientShardName;

    before(function () {
        st = new ShardingTest({
            mongos: 1,
            shards: 2,
            other: {
                auth: "",
                keyFile: kKeyFile,
                rsOptions: {
                    setParameter: {
                        ingressRequestRateLimiterEnabled: 0,
                        reshardingMinimumOperationDurationMillis: 0,
                    },
                },
            },
        });

        st.s.getDB("admin").createUser({user: kUser, pwd: kPass, roles: ["root"]});
        exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        exemptConn.getDB("admin").auth(kUser, kPass);

        assert.commandWorked(exemptConn.adminCommand({enableSharding: "test"}));
        assert.commandWorked(
            exemptConn.adminCommand({shardCollection: "test.coll", key: {oldKey: 1}}),
        );

        configPrimary = st.configRS.getPrimary();
        shardPrimaries = [st.rs0.getPrimary(), st.rs1.getPrimary()];
        recipientShardName = st.shard1.shardName;
    });

    after(function () {
        st.stop();
    });

    // Safety net: disable IRRL on all shards after every test regardless of how the test exited.
    afterEach(function () {
        disableIRRLOnAllShards();
    });

    // Drops test.coll and recreates it sharded on {oldKey:1} with kNumDocs documents.
    // oldKey and newKey are equal per document so integrity can be verified regardless of
    // whether resharding committed (collection now on newKey) or aborted (still on oldKey).
    function resetCollection() {
        exemptConn.getDB("test").coll.drop();
        assert.commandWorked(
            exemptConn.adminCommand({shardCollection: "test.coll", key: {oldKey: 1}}),
        );
        const coll = exemptConn.getDB("test").coll;
        for (let i = 0; i < kNumDocs; i++) {
            assert.commandWorked(coll.insert({oldKey: i, newKey: i}));
        }
    }

    // Asserts all kNumDocs documents are present.
    function assertDocumentsIntact() {
        const coll = exemptConn.getDB("test").coll;
        assert.eq(coll.countDocuments({}), kNumDocs, "document count mismatch");
        for (let i = 0; i < kNumDocs; i++) {
            assert.eq(coll.countDocuments({oldKey: i, newKey: i}), 1, {msg: "missing document", i});
        }
    }

    // Starts a reshardCollection Thread and returns it already running.
    function startReshardThread() {
        const thread = new Thread(
            runResharding,
            st.s.host,
            kRateLimiterExemptAppName,
            kUser,
            kPass,
            recipientShardName,
        );
        thread.start();
        return thread;
    }

    // Disables IRRL on every shard primary. Called from afterEach (safety net) and from
    // each test's finally block (primary cleanup) and within tests that must clear the
    // limiter before letting resharding proceed.
    function disableIRRLOnAllShards() {
        shardPrimaries.forEach((node) => disableRateLimiter(node.host));
    }

    // Verifies IRRL is actively rejecting on the shard primaries by routing a find from
    // exemptConn (mongos) through NetworkInterfaceTL-TaskExecutorPool (not in kExemptions)
    // to the donor shard. With zero-burst config the very first attempt is rejected.
    function assertIRRLActiveOnShards() {
        assert.commandFailedWithCode(
            exemptConn.getDB("test").runCommand({find: "coll", filter: {oldKey: 0}, limit: 1}),
            ErrorCodes.IngressRequestRateLimitExceeded,
            "IRRL must actively reject TaskExecutorPool connections after zero-burst enable",
        );
    }

    // ---- Phase 1: document cloning -------------------------------------------------------------

    it("retries IngressRequestRateLimitExceeded on initializing scatter-gather and commits", function () {
        resetCollection();

        // Pause the coordinator before initialization. Enable a zero-burst IRRL on shard
        // nodes while the coordinator is paused. The initialization scatter-gather uses
        // NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (exempt) and succeeds.
        // Subsequent document cloning opens TaskExecutorPool connections from recipient to
        // donor shards; those are not exempt and receive IngressRequestRateLimitExceeded. The cloning layer retries with
        // exponential backoff until the IRRL is cleared.
        const pauseFp = configureFailPoint(
            configPrimary,
            "reshardingPauseCoordinatorBeforeInitializing",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        const pollConn = makeKeyfileExemptConn(shardPrimaries[1].host);
        // Snapshot totalLinesWritten before enabling IRRL. This anchors the log search to
        // entries written after IRRL was engaged, preventing false positives from 5269300
        // entries left in the ring buffer by earlier test iterations.
        const baselineTotalLines = pollConn.adminCommand({getLog: "global"}).totalLinesWritten;

        shardPrimaries.forEach(enableZeroBurstRateLimiter);
        assertIRRLActiveOnShards();
        pauseFp.off();

        try {
            // Wait until at least one IRRL-triggered cloner retry (log 5269300) appears on
            // the recipient. Once confirmed the spike is real; clear the limiter so the
            // next retry can succeed.
            assert.soon(
                () => {
                    const res = pollConn.adminCommand({getLog: "global"});
                    if (!res.ok) return false;
                    const bufferStart = res.totalLinesWritten - res.log.length;
                    return res.log
                        .slice(Math.max(0, baselineTotalLines - bufferStart))
                        .some((line) => {
                            try {
                                const entry = JSON.parse(line);
                                return (
                                    entry.id === 5269300 &&
                                    typeof entry.attr?.error === "string" &&
                                    entry.attr.error.startsWith("IngressRequestRateLimitExceeded")
                                );
                            } catch (e) {
                                return false;
                            }
                        });
                },
                "Expected at least one IRRL-triggered cloner retry (log 5269300) on recipient",
                30000,
            );
            disableIRRLOnAllShards();

            assert.commandWorked(
                reshardThread.returnData(),
                "resharding must commit after transient IngressRequestRateLimitExceeded during initialization",
            );
        } finally {
            disableIRRLOnAllShards();
            try {
                reshardThread.returnData();
            } catch (e) {}
        }
        assertDocumentsIntact();
    });

    // ---- Phase 2: index builds -----------------------------------------------------------------

    it("resharding encounters IngressRequestRateLimitExceeded during index builds and commits with correct index count", function () {
        resetCollection();

        // Add a secondary index whose leading field is oldKey so it is NOT a prefix cover
        // for the new shard key {newKey:1}. The recipient must therefore build a separate
        // newKey_1 shard key index, making the full index set independently verifiable.
        assert.commandWorked(exemptConn.getDB("test").coll.createIndex({oldKey: 1, newKey: 1}));

        // After resharding to {newKey:1} the collection must carry all indexes from the
        // source collection plus the new shard key index:
        //   _id_              (always present)
        //   oldKey_1          (original shard key, preserved as a secondary index)
        //   oldKey_1_newKey_1 (secondary index carried over from source)
        //   newKey_1          (new shard key index built by the recipient)
        const kExpectedIndexCount = 4;
        const pauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeBuildingIndex",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        // getCollectionOptions and index build commands route through ShardRemote::_runCommand →
        // getFixedExecutor() → NetworkInterfaceTL-Sharding-Fixed, which is in kExemptions.
        // IRRL never rejects these connections; the test confirms resharding commits with the
        // correct index set regardless of IRRL being active on shard nodes at this boundary.
        shardPrimaries.forEach(enableZeroBurstRateLimiter);
        assertIRRLActiveOnShards();
        pauseFp.off();

        try {
            assert.commandWorked(
                reshardThread.returnData(),
                "resharding must commit with IRRL active during index builds",
            );
        } finally {
            disableIRRLOnAllShards();
            try {
                reshardThread.returnData();
            } catch (e) {}
        }
        assertDocumentsIntact();

        const indexes = exemptConn.getDB("test").coll.getIndexes();
        assert.eq(indexes.length, kExpectedIndexCount, {
            msg: "index count mismatch after resharding",
            indexes,
        });
        assert(
            indexes.some((idx) => tojson(idx.key) === tojson({newKey: 1})),
            "new shard key index missing after resharding",
            {indexes},
        );
        assert(
            indexes.some((idx) => tojson(idx.key) === tojson({oldKey: 1, newKey: 1})),
            "secondary index not carried over after resharding",
            {indexes},
        );
    });

    // ---- Phase 3: oplog application ------------------------------------------------------------

    it("IRRL active during oplog application does not disrupt resharding because oplog fetch uses exempt Sharding-Fixed connections", function () {
        resetCollection();

        const pauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeOplogApplication",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        // ReshardingOplogFetcher::consume calls shard->runAggregation, which routes through
        // ShardRemote::_runAggregation → getFixedExecutor() → NetworkInterfaceTL-Sharding-Fixed.
        // That executor is in kExemptions, so IRRL never rejects oplog fetch connections. The
        // test verifies resharding commits normally despite IRRL being active on shard nodes.
        shardPrimaries.forEach(enableZeroBurstRateLimiter);
        assertIRRLActiveOnShards();
        pauseFp.off();

        try {
            assert.commandWorked(
                reshardThread.returnData(),
                "resharding must commit with IRRL active during oplog application",
            );
        } finally {
            disableIRRLOnAllShards();
            try {
                reshardThread.returnData();
            } catch (e) {}
        }
        assertDocumentsIntact();
    });

    // ---- Phase 4: critical section / commit ----------------------------------------------------

    it("critical section commits despite IRRL because coordinator connections are exempt", function () {
        resetCollection();

        // Pause the coordinator at the critical section entry point (after initialization
        // and cloning are complete). Enable zero-burst IRRL on shard nodes, then release
        // the pause. All coordinator operations in the critical section use
        // NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork, covered by the
        // "NetworkInterfaceTL-Resharding" prefix in the exemption list. Resharding
        // therefore commits normally without any IngressRequestRateLimitExceeded errors on the coordinator path.
        const pauseFp = configureFailPoint(
            configPrimary,
            "reshardingPauseCoordinatorBeforeBlockingWrites",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        shardPrimaries.forEach(enableZeroBurstRateLimiter);
        assertIRRLActiveOnShards();
        pauseFp.off();

        try {
            assert.commandWorked(
                reshardThread.returnData(),
                "critical section must commit because coordinator connections are exempt",
            );
        } finally {
            disableIRRLOnAllShards();
            try {
                reshardThread.returnData();
            } catch (e) {}
        }
        assertDocumentsIntact();
    });

    // ---- Abort path ----------------------------------------------------------------------------

    it("aborts cleanly via abortReshardCollection while IRRL is active", function () {
        resetCollection();

        // Pause the recipient's collection cloner before each attempt. This fires on the
        // recipient (rs1) and gives a deterministic point at which to engage the IRRL.
        const clonerPauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingCollectionClonerPauseBeforeAttempt",
        );
        const reshardThread = startReshardThread();
        clonerPauseFp.wait();

        shardPrimaries.forEach(enableZeroBurstRateLimiter);
        assertIRRLActiveOnShards();

        // Issue abortReshardCollection in a Thread so we can concurrently release the cloner.
        // abortReshardCollection routes via NetworkInterfaceTL-ReshardingCoordinatorService-
        // Network (exempt), so the command reaches the config server despite IRRL on shards.
        const abortReshardThread = new Thread(
            function (host, appName, user, pass) {
                const conn = new Mongo(`mongodb://${host}/?appName=${appName}`);
                conn.getDB("admin").auth(user, pass);
                return conn.adminCommand({abortReshardCollection: "test.coll"});
            },
            st.s.host,
            kRateLimiterExemptAppName,
            kUser,
            kPass,
        );

        let abortThreadStarted = false;
        try {
            abortReshardThread.start();
            abortThreadStarted = true;
            // Release the cloner so the abort's blocking getNoThrow can resolve. The abort
            // fires quickly via the coordinator's exempt connections, cancelling the recipient's
            // abort token before the cloner can make progress under IRRL.
            clonerPauseFp.off();

            assert.commandWorked(
                abortReshardThread.returnData(),
                "abortReshardCollection must succeed even while IRRL is active",
            );
            assert.commandFailedWithCode(
                reshardThread.returnData(),
                [ErrorCodes.ReshardCollectionAborted],
                "resharding must abort when abortReshardCollection is called under IRRL",
            );
        } finally {
            clonerPauseFp.off(); // idempotent safety net
            disableIRRLOnAllShards();
            if (abortThreadStarted) {
                try {
                    abortReshardThread.returnData();
                } catch (e) {}
            }
            try {
                reshardThread.returnData();
            } catch (e) {}
        }

        assertDocumentsIntact();

        // No orphaned temporary resharding collections should survive a clean abort.
        const tempCollections = exemptConn
            .getDB("test")
            .getCollectionNames()
            .filter((n) => n.startsWith("system.resharding"));
        assert.eq(tempCollections.length, 0, {
            msg: "orphaned temp collection found after abort",
            tempCollections,
        });
    });
});
