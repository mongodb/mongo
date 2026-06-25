/**
 * Tests createCollection (unsharded and sharded) under IRRL on config server, coordinator shard,
 * and participant shard.
 *
 * Call stacks (verified in source):
 *
 * Unsharded: mongos → coordinator via TaskExecutorPool (not exempt); coordinator runs
 *   _createUntrackedCollection locally — no config RPCs, no participant RPCs.
 *
 * Sharded: mongos → coordinator via TaskExecutorPool (not exempt); coordinator runs
 *   CreateCollectionCoordinator:
 *   - kEnterCriticalSection / kExitCriticalSection: ShardsvrParticipantBlock to ALL shards
 *     via ShardingCoordinatorNetwork (exempt).
 *   - kCreateCollectionOnParticipants: ShardsvrCreateCollectionParticipant to shards with
 *     chunks via TaskExecutorPool (not exempt). Fresh collection: all chunks on coordinator,
 *     so participant is skipped.
 *   - kCommitOnShardingCatalog: SEPTransactionClient on coordinator's router role → config
 *     server. Separate connection pool from ShardingCoordinatorNetwork (not exempt at config).
 *   _mustAlwaysMakeProgress = true for phase >= kEnterWriteCriticalSectionOnCoordinator.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    disableRateLimiter,
    enableZeroBurstRateLimiter,
    getRateLimiterStats,
    kExemptions,
    makeExemptConn,
    makeKeyfileExemptConn,
    kRateLimiterExemptAppName,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kKeyFile = "jstests/libs/key1";
const kUser = "admin";
const kPass = "pwd";
const kDb = "create_coll_irrl_test";
const kHangWindowMs = 8000;
// Pre-sharded collection used by assertIrrlActive: {_id: -1} routes to coordinator shard,
// {_id: 1} routes to participant shard, both via mongos TaskExecutorPool (not exempt).
const kProbeColl = "probe";

// Routes via mongos → shard TaskExecutorPool (not exempt at the shard). With near-zero burst
// the shard rejects immediately; mongos retries then propagates 462.
function assertIrrlActive(mongosConn, filter) {
    assert.commandFailedWithCode(
        mongosConn.getDB(kDb).runCommand({find: kProbeColl, filter}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "mongos TaskExecutorPool must be rejected — IRRL token bucket is empty",
    );
}

function startShardCollection(mongosHost, ns) {
    const thread = new Thread(
        function (mongosHost, appName, user, pass, ns) {
            const conn = new Mongo(`mongodb://${mongosHost}/?appName=${appName}`);
            conn.getDB("admin").auth(user, pass);
            return conn.adminCommand({shardCollection: ns, key: {_id: 1}});
        },
        mongosHost,
        kRateLimiterExemptAppName,
        kUser,
        kPass,
        ns,
    );
    thread.start();
    return thread;
}

describe("createCollection (unsharded + sharded) under IRRL at each cluster node", function () {
    let st;
    let exemptConn;
    let configPrimary;
    let coordinatorShard;
    let participantShard;
    let participantShardName;
    let configExemptConn;
    let coordinatorExemptConn;
    let participantExemptConn;
    let collCounter = 0;

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

        st.s.getDB("admin").createUser({user: kUser, pwd: kPass, roles: ["root"]});
        exemptConn = makeExemptConn(st.s.host);

        assert.commandWorked(exemptConn.adminCommand({enableSharding: kDb}));

        configPrimary = st.configRS.getPrimary();

        const dbInfo = exemptConn.getDB("config").databases.findOne({_id: kDb});
        const primaryShardName = dbInfo ? dbInfo.primary : st.shard0.shardName;
        if (primaryShardName === st.shard0.shardName) {
            coordinatorShard = st.rs0.getPrimary();
            participantShard = st.rs1.getPrimary();
            participantShardName = st.shard1.shardName;
        } else {
            coordinatorShard = st.rs1.getPrimary();
            participantShard = st.rs0.getPrimary();
            participantShardName = st.shard0.shardName;
        }

        jsTestLog(`Config: ${configPrimary.host}`);
        jsTestLog(`Coordinator shard: ${coordinatorShard.host}`);
        jsTestLog(`Participant shard (${participantShardName}): ${participantShard.host}`);

        configExemptConn = makeKeyfileExemptConn(configPrimary.host);
        coordinatorExemptConn = makeKeyfileExemptConn(coordinatorShard.host);
        participantExemptConn = makeKeyfileExemptConn(participantShard.host);

        // Shard the probe collection so assertIrrlActive can target each shard specifically.
        assert.commandWorked(
            exemptConn.adminCommand({shardCollection: `${kDb}.${kProbeColl}`, key: {_id: 1}}),
        );
        assert.commandWorked(
            exemptConn.adminCommand({split: `${kDb}.${kProbeColl}`, middle: {_id: 0}}),
        );
        assert.commandWorked(
            exemptConn.adminCommand({
                moveChunk: `${kDb}.${kProbeColl}`,
                find: {_id: 0},
                to: participantShardName,
            }),
        );
    });

    after(function () {
        st.stop();
    });

    afterEach(function () {
        [configPrimary, coordinatorShard, participantShard].forEach((n) => {
            try {
                disableRateLimiter(n.host);
            } catch (e) {
                throw new Error(`afterEach: failed to disable rate limiter on ${n.host}: ${e}`);
            }
        });
    });

    function nextCollName() {
        return `coll_${collCounter++}`;
    }

    // A1: Unsharded — _createUntrackedCollection makes no config RPCs; collection absent from config.collections.
    // A2: Sharded — kCommitOnShardingCatalog uses SEPTransactionClient (coordinator's router
    //   role), a separate pool from ShardingCoordinatorNetwork. Config IRRL rejects it;
    //   coordinator retries (_mustAlwaysMakeProgress) until IRRL is cleared.
    //   Failpoint on coordinator ensures IRRL is active for the first commit attempt.
    it("Scenario A: IRRL on config server — DDLs that write to the sharding catalog via SEPTransactionClient retry until cleared; DDLs that bypass config are unaffected", function () {
        // A1: Unsharded createCollection
        {
            const collName = nextCollName();
            const ns = `${kDb}.${collName}`;
            enableZeroBurstRateLimiter(configPrimary, kExemptions);
            const result = exemptConn.getDB(kDb).createCollection(collName);
            jsTestLog(`[A1] Unsharded create: ${result.ok ? "OK" : "FAIL"}`, {result});
            assert.commandWorked(
                result,
                "[A1] Unsharded createCollection must succeed — _createUntrackedCollection makes no config RPCs",
            );
            // _createUntrackedCollection never writes to config.collections. Checking catalog
            // state directly avoids the race where background cluster traffic increments the
            // config rejection counter between snapshots.
            assert.eq(
                configExemptConn.getDB("config").collections.findOne({_id: ns}),
                null,
                "[A1] Collection must not appear in config.collections — _createUntrackedCollection bypasses the sharding catalog",
            );
            disableRateLimiter(configPrimary.host);
        }

        // A2: Sharded createCollection
        {
            const collName = nextCollName();
            const ns = `${kDb}.${collName}`;

            // Pause the coordinator just before the config commit so IRRL is active on the first attempt.
            jsTestLog(
                `[A2] Setting hangBeforeCommitOnShardingCatalog on coordinator shard: ${coordinatorShard.host}`,
            );
            const hangFp = configureFailPoint(
                coordinatorShard,
                "hangBeforeCommitOnShardingCatalog",
            );

            const shardThread = startShardCollection(st.s.host, ns);
            try {
                hangFp.wait();

                jsTestLog("[A2] Coordinator reached commit phase. Enabling IRRL on config server.");
                // No mongos-routed path reaches config IRRL — config only receives internal cluster
                // RPCs (ShardingCoordinatorNetwork / SEPTransactionClient). The assert.soon below
                // is the proof that IRRL was active: it polls until the coordinator's rejected
                // admission count rises.
                enableZeroBurstRateLimiter(configPrimary, kExemptions);
                const rejBefore = getRateLimiterStats(configExemptConn).rejectedAdmissions;

                hangFp.off();
                jsTestLog(
                    "[A2] Failpoint released. Waiting for config server to reject coordinator catalog commit attempts.",
                );
                assert.soon(
                    () => {
                        const rejNow = getRateLimiterStats(configExemptConn).rejectedAdmissions;
                        return rejNow > rejBefore;
                    },
                    "[A2] Timed out waiting for config server to reject coordinator catalog commit attempts",
                    kHangWindowMs,
                    200,
                );
                const rejMid = getRateLimiterStats(configExemptConn).rejectedAdmissions;
                jsTestLog(
                    `[A2] Config server rejections while IRRL active: ${rejMid - rejBefore}`,
                    {
                        rejBefore,
                        rejMid,
                    },
                );

                jsTestLog("[A2] Clearing IRRL on config server — coordinator must now commit.");
                disableRateLimiter(configPrimary.host);

                assert.commandWorked(
                    shardThread.returnData(),
                    "[A2] Sharded createCollection must complete after config IRRL cleared " +
                        "(coordinator retried catalog commit until unblocked)",
                );
            } finally {
                hangFp.off();
                try {
                    disableRateLimiter(configPrimary.host);
                } catch (e) {}
                try {
                    shardThread.join();
                } catch (e) {}
            }
        }
    });

    // Both create paths use DBPrimaryRouter: mongos → coordinator via TaskExecutorPool (not
    // exempt). Coordinator rejects before the command runs; mongos retries a bounded number
    // of times → fast failure for both paths.
    it("Scenario B: IRRL on coordinator shard — DDLs routed via TaskExecutorPool (mongos → coordinator) are rejected immediately; 462 propagates to the client", function () {
        enableZeroBurstRateLimiter(coordinatorShard, kExemptions);
        assertIrrlActive(exemptConn, {_id: -1}); // {_id: -1} routes to coordinator shard

        // B1: Unsharded createCollection
        {
            const collName = nextCollName();
            const rejBefore = getRateLimiterStats(coordinatorExemptConn).rejectedAdmissions;
            const result = exemptConn.getDB(kDb).createCollection(collName);
            const rejAfter = getRateLimiterStats(coordinatorExemptConn).rejectedAdmissions;
            jsTestLog(
                `[B1] Unsharded create: ${result.ok ? "OK" : "FAIL"}, coordinator rejections: ${rejAfter - rejBefore}`,
                {result},
            );
            assert.commandFailedWithCode(
                result,
                ErrorCodes.IngressRequestRateLimitExceeded,
                "[B1] Unsharded createCollection must fail fast — mongos TaskExecutorPool rejected at coordinator shard ingress",
            );
            assert.gt(
                rejAfter,
                rejBefore,
                "[B1] Coordinator shard must have rejected the mongos TaskExecutorPool connection",
            );
        }

        // B2: Sharded createCollection
        {
            const collName = nextCollName();
            const ns = `${kDb}.${collName}`;
            const rejBefore = getRateLimiterStats(coordinatorExemptConn).rejectedAdmissions;
            const result = exemptConn.adminCommand({shardCollection: ns, key: {_id: 1}});
            const rejAfter = getRateLimiterStats(coordinatorExemptConn).rejectedAdmissions;
            jsTestLog(
                `[B2] Sharded create: ${result.ok ? "OK" : "FAIL"}, coordinator rejections: ${rejAfter - rejBefore}`,
                {result},
            );
            assert.commandFailedWithCode(
                result,
                ErrorCodes.IngressRequestRateLimitExceeded,
                "[B2] Sharded createCollection must fail fast — shardCollection uses the same " +
                    "DBPrimaryRouter → TaskExecutorPool path as unsharded create",
            );
            assert.gt(
                rejAfter,
                rejBefore,
                "[B2] Coordinator shard must have rejected the mongos TaskExecutorPool connection",
            );
        }
    });

    // C1: Unsharded — no participant RPCs; DDL succeeds.
    // C2: Sharded — critical section fan-out uses ShardingCoordinatorNetwork (exempt);
    //   kCreateCollectionOnParticipants skips participant (no chunks there); DDL succeeds.
    //   In both cases, IRRL active + DDL success proves no non-exempt participant contact.
    it("Scenario C: IRRL on participant shard — coordinator-to-participant RPCs via ShardingCoordinatorNetwork are exempt; DDL completes unaffected", function () {
        enableZeroBurstRateLimiter(participantShard, kExemptions);
        assertIrrlActive(exemptConn, {_id: 1}); // {_id: 1} routes to participant shard

        // C1: Unsharded createCollection
        {
            const collName = nextCollName();
            const result = exemptConn.getDB(kDb).createCollection(collName);
            jsTestLog(`[C1] Unsharded create: ${result.ok ? "OK" : "FAIL"}`, {result});
            // _createUntrackedCollection sends no RPCs to the participant. IRRL is confirmed
            // active above (assertIrrlActive), so success proves no non-exempt participant
            // contact occurred — any such contact would have returned 462 and failed the DDL.
            assert.commandWorked(
                result,
                "[C1] Unsharded createCollection must succeed — _createUntrackedCollection has no participant RPCs",
            );
        }

        // C2: Sharded createCollection
        {
            const collName = nextCollName();
            const ns = `${kDb}.${collName}`;
            const result = exemptConn.adminCommand({shardCollection: ns, key: {_id: 1}});
            jsTestLog(`[C2] Sharded create: ${result.ok ? "OK" : "FAIL"}`, {result});
            // kEnterCriticalSection / kExitCriticalSection fan out to participant via
            // ShardingCoordinatorNetwork (exempt). kCreateCollectionOnParticipants skips
            // participant (no chunks there). If ShardingCoordinatorNetwork lost its exemption,
            // the critical-section fan-out would be rejected and the DDL would fail — success
            // here proves the exempt path held.
            assert.commandWorked(
                result,
                "[C2] Sharded createCollection must succeed — ShardingCoordinatorNetwork is exempt " +
                    "(critical section fan-out passes through participant IRRL)",
            );
        }
    });

    // Covers kCreateCollectionOnParticipants — the exposure point C2 skips because a fresh
    // sharded collection places all chunks on the coordinator. Hashed key + numInitialChunks=4
    // distributes chunks across both shards, so kCreateCollectionOnParticipants must contact
    // the participant via TaskExecutorPool (not ShardingCoordinatorNetwork, which is exempt).
    // IRRL rejects those RPCs; coordinator retries (_mustAlwaysMakeProgress) until cleared.
    it("Scenario D: IRRL on participant shard — coordinator-to-participant RPCs via TaskExecutorPool are retried until cleared; DDL eventually succeeds", function () {
        const collName = nextCollName();
        const ns = `${kDb}.${collName}`;

        jsTestLog(`[D] Enabling IRRL on participant shard: ${participantShard.host}`);
        enableZeroBurstRateLimiter(participantShard, kExemptions);
        assertIrrlActive(exemptConn, {_id: 1}); // {_id: 1} routes to participant shard

        const rejBefore = getRateLimiterStats(participantExemptConn).rejectedAdmissions;

        // Hashed shard key with numInitialChunks forces MongoDB to pre-split and distribute
        // chunks across all shards. With 2 shards and numInitialChunks=4, each shard gets
        // ~2 chunks → participant shard is included in kCreateCollectionOnParticipants.
        const shardThread = new Thread(
            function (mongosHost, appName, user, pass, ns) {
                const conn = new Mongo(`mongodb://${mongosHost}/?appName=${appName}`);
                conn.getDB("admin").auth(user, pass);
                return conn.adminCommand({
                    shardCollection: ns,
                    key: {_id: "hashed"},
                    numInitialChunks: 4,
                });
            },
            st.s.host,
            kRateLimiterExemptAppName,
            kUser,
            kPass,
            ns,
        );
        shardThread.start();

        try {
            jsTestLog(
                "[D] Waiting for coordinator kCreateCollectionOnParticipants retries on participant shard.",
            );
            assert.soon(
                () => {
                    const rejNow = getRateLimiterStats(participantExemptConn).rejectedAdmissions;
                    return rejNow > rejBefore;
                },
                "[D] Timed out waiting for participant shard to reject kCreateCollectionOnParticipants RPCs",
                kHangWindowMs,
                200,
            );
            const rejMid = getRateLimiterStats(participantExemptConn).rejectedAdmissions;
            jsTestLog(`[D] Participant shard rejections while IRRL active: ${rejMid - rejBefore}`, {
                rejBefore,
                rejMid,
            });

            jsTestLog(
                "[D] Clearing IRRL on participant shard — coordinator must now complete kCreateCollectionOnParticipants.",
            );
            disableRateLimiter(participantShard.host);

            assert.commandWorked(
                shardThread.returnData(),
                "[D] shardCollection must succeed after IRRL cleared " +
                    "(coordinator retried kCreateCollectionOnParticipants until TaskExecutorPool unblocked)",
            );
        } finally {
            try {
                disableRateLimiter(participantShard.host);
            } catch (e) {}
            try {
                shardThread.join();
            } catch (e) {}
        }
    });
});
