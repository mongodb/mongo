/**
 * Tests ingress request rate limiter (IRRL) behavior for sharded transactions and CRUD routing.
 *
 * With a near-zero token-bucket rate (5e-6 tokens/sec, burst capacity = 1/rate), effective max
 * tokens per shard is ~1. Any connection whose app name is not in the exemption list is rejected
 * after that single token is consumed.
 *
 * Test cases:
 *
 *   1. Non-exempt CRUD routed from mongos to shards:
 *      Finds targeting individual shards use NetworkInterfaceTL-TaskExecutorPool-{i}, which is
 *      intentionally absent from the exemption list. After draining the token, subsequent finds to
 *      the same shard fail with IngressRequestRateLimitExceeded. Mongos retries up to
 *      defaultClientMaxRetryAttempts (default 3) times with jittered backoff before propagating
 *      the error back to the client.
 *
 *   2. Transaction CRUD rate-limited, causing transaction failure:
 *      CRUD operations within a transaction also route through TaskExecutorPool (not exempt). With
 *      the token exhausted, the first CRUD op in a new transaction fails after mongos retries,
 *      leaving the transaction open on the server. abortTransaction succeeds because it uses
 *      NetworkInterfaceTL-Sharding-Fixed (exempt).
 *
 *   3. Single-shard transaction commit succeeds:
 *      CRUD is done before enabling the rate limiter. The commit phase uses
 *      NetworkInterfaceTL-Sharding-Fixed (via sendCommitDirectlyToShards in
 *      transaction_router.cpp), which is on the exemption list.
 *
 *   4. Cross-shard two-phase commit transaction succeeds:
 *      Same as above, but 2PC is involved. Both the coordinateCommit RPC from mongos and the
 *      commitTransaction fan-out from the coordinator shard to participants use
 *      NetworkInterfaceTL-Sharding-Fixed, so the commit succeeds despite the overloaded limiter.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    kExemptions,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
    makeExemptConn,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kKeyFile = "jstests/libs/key1";
const kUser = "admin";
const kPass = "pwd";

function enableOverloadedRateLimiter(conn) {
    authutil.asCluster(conn, kKeyFile, () => {
        configureFailPoint(conn, "ingressRequestRateLimiterFractionalRateOverride", {
            rate: kSlowestRefreshRateSecs,
        });
        assert.commandWorked(
            conn.adminCommand({
                setParameter: 1,
                ingressRequestAdmissionRatePerSec: 1,
                ingressRequestAdmissionBurstCapacitySecs: Math.round(1.0 / kSlowestRefreshRateSecs),
                ingressRequestRateLimiterApplicationExemptions: {appNames: kExemptions},
                ingressRequestRateLimiterEnabled: 1,
            }),
        );
    });
}

function disableRateLimiter(host) {
    // Use a fresh unauthenticated connection with the exempt appName. asCluster authenticates
    // as __system (keyFile), which is available on all cluster members regardless of user setup.
    // admin/pwd does not exist in a shard's local user store — only on the config server.
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

// Sends a targeted find through the non-exempt TaskExecutorPool path to consume the last
// available token on the shard containing filter, then asserts that an identical follow-up
// request is rejected, proving the bucket is now fully exhausted.
function drainToken(db, filter) {
    db.runCommand({find: "coll", filter, limit: 1});
    assert.commandFailedWithCode(
        db.runCommand({find: "coll", filter, limit: 1}),
        ErrorCodes.IngressRequestRateLimitExceeded,
        "token must be exhausted after drain",
    );
}

describe("sharded transactions with ingress rate limiter active", function () {
    let st;
    let exemptConn;
    let rateLimitedNodes;

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

        // Create a sharded collection with the range [MinKey, 0) on shard0 and [0, MaxKey) on
        // shard1 so that transactions targeting negative _id values are single-shard and those
        // spanning negative and positive _id values require two-phase commit.
        assert.commandWorked(exemptConn.adminCommand({enableSharding: "test"}));
        assert.commandWorked(exemptConn.adminCommand({shardCollection: "test.coll", key: {_id: 1}}));
        assert.commandWorked(exemptConn.adminCommand({split: "test.coll", middle: {_id: 0}}));
        assert.commandWorked(
            exemptConn.adminCommand({
                moveChunk: "test.coll",
                find: {_id: 1},
                to: st.shard1.shardName,
            }),
        );

        rateLimitedNodes = [st.configRS.getPrimary(), st.rs0.getPrimary(), st.rs1.getPrimary()];
    });

    after(function () {
        st.stop();
    });

    it("non-exempt CRUD routed through mongos is rate-limited on shard primaries", function () {
        const db = exemptConn.getDB("test");

        // Finds routed from mongos to a shard arrive with app name
        // NetworkInterfaceTL-TaskExecutorPool-{i}, which is intentionally absent from the
        // exemption list. With the token bucket at ~1, one drain request is enough to empty it;
        // every subsequent non-exempt request is rejected immediately.

        rateLimitedNodes.forEach(enableOverloadedRateLimiter);
        try {
            drainToken(db, {_id: -20}); // shard0: range [MinKey, 0)
            drainToken(db, {_id: 20}); // shard1: range [0, MaxKey)
        } finally {
            rateLimitedNodes.forEach((p) => disableRateLimiter(p.host));
        }
    });

    it("transaction CRUD fails with IngressRequestRateLimitExceeded when shard token is exhausted", function () {
        const db = exemptConn.getDB("test");
        const session = exemptConn.startSession();

        rateLimitedNodes.forEach(enableOverloadedRateLimiter);
        try {
            // Exhaust the token on the shard holding [MinKey, 0); the transaction CRUD
            // below confirms the bucket is still empty after the drain.
            drainToken(db, {_id: -30});

            const sessionDB = session.getDatabase("test");
            session.startTransaction();

            // CRUD within a transaction uses NetworkInterfaceTL-TaskExecutorPool (not
            // exempt). After retries, mongos propagates the rate limit error back.
            assert.commandFailedWithCode(
                sessionDB.runCommand({find: "coll", filter: {_id: -30}, limit: 1}),
                ErrorCodes.IngressRequestRateLimitExceeded,
                "transaction CRUD must be rejected after the shard's ingress token is exhausted",
            );

            // The transaction remains open on the server. abortTransaction uses
            // NetworkInterfaceTL-Sharding-Fixed (exempt) and succeeds under rate limiting.
            session.abortTransaction();
        } finally {
            session.endSession();
            rateLimitedNodes.forEach((p) => disableRateLimiter(p.host));
        }
    });

    it("single-shard transaction commit succeeds with overloaded rate limiter", function () {
        const coll = exemptConn.getDB("test").coll;
        assert.commandWorked(
            coll.insertMany([
                {_id: -10, val: "a"},
                {_id: -11, val: "b"},
            ]),
        );

        // Perform CRUD within the transaction before enabling the rate limiter on shard primaries.
        // Only the commit phase runs with rate limiting active, directly targeting the
        // NetworkInterfaceTL-Sharding-Fixed exemption used by sendCommitDirectlyToShards().
        const session = exemptConn.startSession();
        const sessionColl = session.getDatabase("test").coll;
        session.startTransaction();
        assert.commandWorked(sessionColl.updateOne({_id: -10}, {$set: {committed: true}}));
        assert.commandWorked(sessionColl.updateOne({_id: -11}, {$set: {committed: true}}));

        rateLimitedNodes.forEach(enableOverloadedRateLimiter);
        try {
            // Exhaust the token on the participant shard: commitTransaction must go through
            // the Sharding-Fixed exemption to succeed, not just consume a remaining token.
            drainToken(exemptConn.getDB("test"), {_id: -10});
            session.commitTransaction();
        } finally {
            rateLimitedNodes.forEach((p) => disableRateLimiter(p.host));
        }

        session.endSession();
        assert.eq(coll.findOne({_id: -10}).committed, true, "doc -10 not committed");
        assert.eq(coll.findOne({_id: -11}).committed, true, "doc -11 not committed");
    });

    it("cross-shard two-phase commit transaction succeeds with overloaded rate limiter", function () {
        const coll = exemptConn.getDB("test").coll;
        assert.commandWorked(
            coll.insertMany([
                {_id: -1, val: "shard0"},
                {_id: 1, val: "shard1"},
            ]),
        );

        // Both shards participate in this transaction, triggering the 2PC coordinator path.
        // Perform CRUD against both shards before enabling rate limiting, then commit.
        // The 2PC commit uses NetworkInterfaceTL-Sharding-Fixed on:
        //   - mongos: to send coordinateCommit to the coordinator shard
        //   - coordinator shard mongod: to send commitTransaction to participant shards
        const session = exemptConn.startSession();
        const sessionColl = session.getDatabase("test").coll;
        session.startTransaction();
        assert.commandWorked(sessionColl.updateOne({_id: -1}, {$set: {committed: true}}));
        assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {committed: true}}));

        rateLimitedNodes.forEach(enableOverloadedRateLimiter);
        try {
            // Exhaust the token on each participant shard; both must be empty since the 2PC
            // coordinator fans out commitTransaction to each via Sharding-Fixed.
            drainToken(exemptConn.getDB("test"), {_id: -1});
            drainToken(exemptConn.getDB("test"), {_id: 1});
            session.commitTransaction();
        } finally {
            rateLimitedNodes.forEach((p) => disableRateLimiter(p.host));
        }

        session.endSession();
        assert.eq(coll.findOne({_id: -1}).committed, true, "doc -1 not committed");
        assert.eq(coll.findOne({_id: 1}).committed, true, "doc 1 not committed");
    });
});
