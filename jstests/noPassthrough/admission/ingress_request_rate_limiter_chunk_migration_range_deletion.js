/**
 * Tests that chunk migrations and range deletions are immune to IRRL on donor and recipient shards.
 *
 * moveChunk routing:
 *   mongos → config server (_configsvrMoveRange via shard registry)
 *   config server → donor (_shardsvrMoveRange via BalancerCommandsSchedulerImpl →
 *     getFixedExecutor → NetworkInterfaceTL-Sharding-Fixed, EXEMPT at donor)
 *   donor → recipient (_recvChunk* via MigrationChunkClonerSource::_callRecipient →
 *     getFixedExecutor → NetworkInterfaceTL-Sharding-Fixed, EXEMPT at recipient)
 *
 * Range deletion: RangeDeleterService::deleteRangeInBatches runs locally via DBDirectClient
 *   (no inbound network RPCs); immune to IRRL.
 * Donor → recipient admin RPCs (mark task ready, delete task) use getFixedExecutor →
 *   Sharding-Fixed (EXEMPT at recipient).
 *
 * IRRL on donor or recipient does not block migrations or range deletions.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
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
const kDb = "migration_irrl_test";
// Pre-sharded probe collection: {_id: -1} routes to coordinator shard, {_id: 1} routes to
// participant shard, both via mongos TaskExecutorPool (not exempt). Used to confirm IRRL is active.
const kProbeColl = "probe";

function makeKeyfileExemptConn(host) {
    const conn = new Mongo(`mongodb://${host}/?appName=${kRateLimiterExemptAppName}`);
    authutil.assertAuthenticate(conn, "admin", {
        user: "__system",
        mechanism: "SCRAM-SHA-256",
        pwd: cat(kKeyFile).replace(/[\011-\015\040]/g, ""),
    });
    return conn;
}

// Routes via mongos → shard TaskExecutorPool (not exempt). With near-zero burst the shard rejects
// immediately; confirms IRRL is active on the shard that owns the chunk for `filter`.
function assertIrrlActive(mongosConn, filter) {
    assert.commandFailedWithCode(
        mongosConn.getDB(kDb).runCommand({find: kProbeColl, filter}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "mongos TaskExecutorPool must be rejected — IRRL token bucket is empty",
    );
}

function enableZeroBurstRateLimiter(conn, exemptions) {
    authutil.asCluster(conn, kKeyFile, () => {
        configureFailPoint(conn, "ingressRequestRateLimiterFractionalRateOverride", {
            rate: kSlowestRefreshRateSecs,
        });
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: 1,
                ingressRequestAdmissionBurstCapacitySecs: kSlowestRefreshRateSecs,
                ingressRequestRateLimiterApplicationExemptions: {appNames: exemptions},
                ingressRequestRateLimiterEnabled: 1,
            }),
        );
    });
}

function disableRateLimiter(host) {
    const conn = new Mongo(`mongodb://${host}/?appName=${kRateLimiterExemptAppName}`);
    authutil.asCluster(conn, kKeyFile, () => {
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: 1,
                ingressRequestAdmissionBurstCapacitySecs: kSlowestRefreshRateSecs,
                ingressRequestRateLimiterEnabled: 0,
            }),
        );
    });
}

describe("chunk migration and range deletion under IRRL", function () {
    let st;
    let exemptConn;
    let configPrimary;
    let coordinatorShard;
    let participantShard;
    let participantShardName;
    let coordinatorExemptConn;

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
        exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        exemptConn.getDB("admin").auth(kUser, kPass);

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
        jsTestLog(`Coordinator shard (donor): ${coordinatorShard.host}`);
        jsTestLog(
            `Participant shard (recipient, ${participantShardName}): ${participantShard.host}`,
        );

        coordinatorExemptConn = makeKeyfileExemptConn(coordinatorShard.host);

        // Shard the probe collection with a chunk on each shard for assertIrrlActive.
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

    it("migration proceeds and range deletion completes despite IRRL active on both donor and recipient", function () {
        const ns = `${kDb}.test`;

        // Shard the collection; insert a doc that will become an orphan on the donor after the move.
        assert.commandWorked(exemptConn.adminCommand({shardCollection: ns, key: {_id: 1}}));
        exemptConn.getDB(kDb).test.insertOne({_id: -1});

        // Suspend range deletion on the donor so the orphaned range deletion task persists in
        // config.rangeDeletions after moveChunk, letting us verify its immunity to IRRL separately.
        const suspendFp = configureFailPoint(coordinatorExemptConn, "suspendRangeDeletion");
        try {
            // Enable IRRL on both donor and recipient with near-zero burst. The config server is
            // intentionally left without IRRL: mongos dispatches _configsvrMoveRange to the config
            // server via TaskExecutorPool (not in kExemptions), so IRRL on the config server would
            // reject it with 462 before the migration starts — identical to the generic DDL
            // Scenario B covered in ingress_request_rate_limiter_ddl_create_collection.js.
            enableZeroBurstRateLimiter(coordinatorShard, kExemptions);
            enableZeroBurstRateLimiter(participantShard, kExemptions);

            // Confirm IRRL is active on both shards.
            assertIrrlActive(exemptConn, {_id: -1}); // coordinator owns [MinKey, 0)
            assertIrrlActive(exemptConn, {_id: 1}); // participant owns [0, MaxKey)

            // Move chunk from coordinator (donor) to participant (recipient).
            // Config server → donor: _shardsvrMoveRange via getFixedExecutor (Sharding-Fixed, exempt).
            // Donor → recipient: _recvChunk* via MigrationChunkClonerSource::_callRecipient →
            //   getFixedExecutor (Sharding-Fixed, exempt). Neither end is subject to IRRL.
            assert.commandWorked(
                exemptConn.adminCommand({moveChunk: ns, find: {_id: -1}, to: participantShardName}),
                "moveChunk must succeed despite IRRL on both donor and recipient",
            );

            // The orphaned doc's range deletion task is held in config.rangeDeletions by suspendFp.
            assert.soon(
                () =>
                    coordinatorExemptConn.getDB("config").rangeDeletions.countDocuments({nss: ns}) >
                    0,
                "range deletion task must exist in config.rangeDeletions after moveChunk",
                5000,
                200,
            );

            // Release range deletion. RangeDeleterService::deleteRangeInBatches runs locally via
            // DBDirectClient — no inbound network RPCs → not subject to IRRL at donor.
            suspendFp.off();

            assert.soon(
                () =>
                    coordinatorExemptConn
                        .getDB("config")
                        .rangeDeletions.countDocuments({nss: ns}) === 0,
                "range deletion must complete despite IRRL on donor (local execution is immune)",
            );
        } finally {
            try {
                suspendFp.off();
            } catch (e) {}
        }
    });
});
