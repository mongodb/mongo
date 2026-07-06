/**
 * Tests resharding correctness under the ingress request rate limiter (IRRL).
 *
 * Index builds and oplog fetch route through NetworkInterfaceTL-Sharding-Fixed (exempt).
 * Document cloning uses NetworkInterfaceTL-TaskExecutorPool (not exempt) and retries on
 * IngressRequestRateLimitExceeded (log 5269300) until IRRL clears.
 *
 * The coordinator commit/abort cleanup calls resumeMigrations → setAllowChunkOperations, which
 * broadcasts ShardsvrSetAllowChunkOperations to all shards. This broadcast now uses
 * Grid::getExecutorPool()->getFixedExecutor() (NetworkInterfaceTL-Sharding-Fixed, exempt), so
 * the commit/abort path is not rate-limited even when IRRL is active on participant shards.
 *
 * IRRL is configured with zero burst so every non-exempt connection is immediately rejected.
 * Phase-boundary failpoints gate enable/disable to eliminate timing races.
 *
 * Five scenarios, ordered by resharding phase:
 *
 *   1. Cloning: IRRL engaged just before clone connections open. NetworkInterfaceTL-TaskExecutorPool
 *      connections are rejected; cloning retries (log 5269300) until IRRL clears.
 *
 *   2. Index builds: IRRL engaged at reshardingPauseRecipientBeforeBuildingIndex, cleared at
 *      reshardingPauseRecipientBeforeOplogApplication. Index build commands use
 *      NetworkInterfaceTL-Sharding-Fixed (exempt) and complete unaffected.
 *
 *   3. Oplog application: IRRL engaged at reshardingPauseRecipientBeforeOplogApplication, cleared
 *      at reshardingPauseRecipientBeforeEnteringStrictConsistency. Oplog fetch uses
 *      NetworkInterfaceTL-Sharding-Fixed (exempt) and completes unaffected.
 *
 *   4. Critical section and commit: IRRL engaged at reshardingPauseCoordinatorBeforeBlockingWrites
 *      and kept active through returnData(). All coordinator RPCs in the critical section and
 *      commit path use NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (exempt via the
 *      NetworkInterfaceTL-Reshard prefix) and setAllowChunkOperations uses Sharding-Fixed (exempt),
 *      so the full commit completes without clearing IRRL.
 *
 *   5. Abort: IRRL engaged mid-cloning, abortReshardCollection issued via exempt connection.
 *      IRRL remains active through both returnData() calls. All coordinator abort RPCs use
 *      NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (exempt), so the full abort
 *      completes without clearing IRRL.
 *
 * KNOWN TEST GAP — participant calculation phase:
 *   calculateParticipantShardsAndChunks routes through NetworkInterfaceTL-TaskExecutorPool on
 *   the config server but cannot be tested deterministically: by the time the test
 *   runs, existing pooled connections are reused regardless of IRRL state.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    disableRateLimiter,
    enableZeroBurstRateLimiter,
    kExemptions,
    kRateLimiterExemptAppName,
    makeKeyfileExemptConn,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kKeyFile = "jstests/libs/key1";
const kUser = "admin";
const kPass = "pwd";
const kNumDocs = 10;

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

    // Disable IRRL before and after each test so DDL in resetCollection() and resmoke's
    // inter-test FCV/feature-flag setParameter calls are never rate-limited.
    beforeEach(function () {
        disableIRRLOnAllShards();
    });

    afterEach(function () {
        disableIRRLOnAllShards();
    });

    // Drops and recreates test.coll sharded on {oldKey:1}. oldKey === newKey per document so
    // integrity checks work whether resharding committed or aborted.
    function resetCollection() {
        exemptConn.getDB("test").coll.drop();
        assert.commandWorked(
            exemptConn.adminCommand({shardCollection: "test.coll", key: {oldKey: 1}}),
        );
        assert.commandWorked(exemptConn.adminCommand({split: "test.coll", middle: {oldKey: 5}}));
        assert.commandWorked(
            exemptConn.adminCommand({
                moveChunk: "test.coll",
                find: {oldKey: 9},
                to: recipientShardName,
            }),
        );
        const coll = exemptConn.getDB("test").coll;
        for (let i = 0; i < kNumDocs; i++) {
            assert.commandWorked(coll.insert({oldKey: i, newKey: i}));
        }
    }

    function assertDocumentsIntact() {
        const coll = exemptConn.getDB("test").coll;
        assert.eq(coll.countDocuments({}), kNumDocs, "document count mismatch");
        for (let i = 0; i < kNumDocs; i++) {
            assert.eq(coll.countDocuments({oldKey: i, newKey: i}), 1, {msg: "missing document", i});
        }
    }

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

    function disableIRRLOnAllShards() {
        shardPrimaries.forEach((node) => disableRateLimiter(node.host));
    }

    // Enables zero-burst IRRL on every shard primary. This cluster starts without the
    // ingressRequestRateLimiterFractionalRateOverride failpoint, so setRefreshRateFailpoint keeps
    // the token bucket effectively empty for the whole time IRRL is enabled.
    function enableIRRLOnAllShards() {
        shardPrimaries.forEach((node) =>
            enableZeroBurstRateLimiter(node, kExemptions, {setRefreshRateFailpoint: true}),
        );
    }

    // Confirms IRRL is actively rejecting by issuing a find that routes through
    // NetworkInterfaceTL-TaskExecutorPool (not exempt) from mongos to each shard.
    function assertIRRLActiveOnShards() {
        const db = exemptConn.getDB("test");
        assert.commandFailedWithCode(
            db.runCommand({find: "coll", filter: {oldKey: 0}, limit: 1}),
            ErrorCodes.IngressRequestRateLimitExceeded,
            "IRRL must actively reject TaskExecutorPool connections after zero-burst enable",
        );
        assert.commandFailedWithCode(
            db.runCommand({find: "coll", filter: {oldKey: 9}, limit: 1}),
            ErrorCodes.IngressRequestRateLimitExceeded,
            "IRRL must actively reject TaskExecutorPool connections on both shards after zero-burst enable",
        );
    }

    // ---- Phase 1: document cloning -------------------------------------------------------------

    it("retries IngressRequestRateLimitExceeded during document cloning and commits", function () {
        resetCollection();

        // Pause at reshardingPauseRecipientBeforeCloning so IRRL is engaged after all init
        // scatter-gathers complete and just before clone connections open. Clone connections
        // use NetworkInterfaceTL-TaskExecutorPool (not exempt) and are rejected; the cloning
        // layer retries until IRRL clears (log 5269300).
        const pauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeCloning",
        );
        // Gate on reshardingPauseRecipientBeforeOplogApplication (fires after cloning and index
        // builds complete) to confirm IRRL was active for the full cloning phase before clearing it.
        const postCloningFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeOplogApplication",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        const pollConn = makeKeyfileExemptConn(shardPrimaries[1].host);
        // Snapshot log position before enabling IRRL to avoid false-positive 5269300 matches
        // from earlier test iterations still in the ring buffer.
        const baselineTotalLines = pollConn.adminCommand({getLog: "global"}).totalLinesWritten;

        enableIRRLOnAllShards();
        assertIRRLActiveOnShards();
        pauseFp.off();

        try {
            // Wait for at least one IRRL-triggered cloner retry (log 5269300).
            assert.soon(() => {
                const res = pollConn.adminCommand({getLog: "global"});
                if (!res.ok) return false;
                const bufferStart = res.totalLinesWritten - res.log.length;
                return res.log.slice(Math.max(0, baselineTotalLines - bufferStart)).some((line) => {
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
            }, "Expected at least one IRRL-triggered cloner retry (log 5269300) on recipient");

            // Cloning is blocked by IRRL, so IRRL must be cleared first to let cloning complete.
            // (postCloningFp cannot be waited on before this: it fires only after cloning finishes,
            // which cannot happen while IRRL is active.) Once IRRL is cleared, waiting on
            // postCloningFp (reshardingPauseRecipientBeforeOplogApplication) is the deterministic
            // boundary confirming cloning actually completed before the test proceeds to
            // returnData(); it is then released.
            disableIRRLOnAllShards();
            postCloningFp.wait();
            postCloningFp.off();

            assert.commandWorked(
                reshardThread.returnData(),
                "resharding must commit after transient IngressRequestRateLimitExceeded during document cloning",
            );
        } finally {
            postCloningFp.off(); // idempotent safety net
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

        // Add a secondary index that is not a prefix cover for the new shard key {newKey:1},
        // forcing the recipient to build a separate newKey_1 index. Expected after resharding:
        //   _id_, oldKey_1, oldKey_1_newKey_1, newKey_1  (4 total)
        assert.commandWorked(exemptConn.getDB("test").coll.createIndex({oldKey: 1, newKey: 1}));
        const kExpectedIndexCount = 4;

        const pauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeBuildingIndex",
        );
        // Gate on reshardingPauseRecipientBeforeOplogApplication to confirm IRRL was active
        // for the full index-build phase before clearing it.
        const postIndexFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeOplogApplication",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        // Index builds route through ShardRemote::_runCommand → NetworkInterfaceTL-Sharding-Fixed
        // (exempt), so IRRL has no effect on this phase.
        enableIRRLOnAllShards();
        assertIRRLActiveOnShards();
        pauseFp.off();

        // IRRL is only exercised during the index-build phase. Disable it before resharding
        // proceeds to the commit phase, which is covered separately by test case 4.
        postIndexFp.wait();
        disableIRRLOnAllShards();
        postIndexFp.off();

        assert.commandWorked(
            reshardThread.returnData(),
            "resharding must commit with IRRL active during index builds",
        );
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
        // Gate on reshardingPauseRecipientBeforeEnteringStrictConsistency (fires after
        // awaitStrictlyConsistent()) to confirm IRRL was active for the full oplog-application
        // phase before clearing it.
        const postOplogFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingPauseRecipientBeforeEnteringStrictConsistency",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        // Oplog fetch routes through ShardRemote::_runAggregation → NetworkInterfaceTL-Sharding-Fixed
        // (exempt), so IRRL has no effect on this phase.
        enableIRRLOnAllShards();
        assertIRRLActiveOnShards();
        pauseFp.off();

        postOplogFp.wait();
        disableIRRLOnAllShards();
        postOplogFp.off();

        assert.commandWorked(
            reshardThread.returnData(),
            "resharding must commit after IRRL active during oplog application",
        );
        assertDocumentsIntact();
    });

    // ---- Phase 4: critical section / commit ----------------------------------------------------

    it("commit completes with IRRL active throughout critical section and commit because all coordinator connections are exempt", function () {
        resetCollection();

        // Pause at reshardingPauseCoordinatorBeforeBlockingWrites so IRRL is active for the
        // entire critical section entry and commit phase. All coordinator RPCs in this phase use
        // NetworkInterfaceTL-ReshardingCoordinatorServiceNetwork (exempt via the
        // NetworkInterfaceTL-Reshard prefix), and setAllowChunkOperations uses Sharding-Fixed
        // (exempt). IRRL stays active through returnData() to verify the full commit path.
        const pauseFp = configureFailPoint(
            configPrimary,
            "reshardingPauseCoordinatorBeforeBlockingWrites",
        );
        const reshardThread = startReshardThread();
        pauseFp.wait();

        enableIRRLOnAllShards();
        assertIRRLActiveOnShards();
        pauseFp.off();

        assert.commandWorked(
            reshardThread.returnData(),
            "resharding must commit with IRRL active throughout critical section and commit",
        );
        disableIRRLOnAllShards();
        assertDocumentsIntact();
    });

    // ---- Abort path ----------------------------------------------------------------------------

    it("abort completes with IRRL active throughout because all coordinator abort connections are exempt", function () {
        resetCollection();

        // Pause mid-cloning to engage IRRL before abort is triggered. All coordinator RPCs in
        // the abort path (_tellAllParticipantsToAbort, _awaitAllParticipantShardsDone, and
        // setAllowChunkOperations) use exempt executors, so IRRL stays active through both
        // returnData() calls to verify the full abort path is exempt.
        const clonerPauseFp = configureFailPoint(
            st.rs1.getPrimary(),
            "reshardingCollectionClonerPauseBeforeAttempt",
        );
        const reshardThread = startReshardThread();
        clonerPauseFp.wait();

        enableIRRLOnAllShards();
        assertIRRLActiveOnShards();

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
            clonerPauseFp.off();

            assert.commandWorked(
                abortReshardThread.returnData(),
                "abortReshardCollection must succeed with IRRL active",
            );
            assert.commandFailedWithCode(
                reshardThread.returnData(),
                [ErrorCodes.ReshardCollectionAborted],
                "resharding must abort when abortReshardCollection is called under IRRL",
            );
            disableIRRLOnAllShards();
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
