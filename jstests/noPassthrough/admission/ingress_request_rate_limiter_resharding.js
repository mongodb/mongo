/**
 * Tests that resharding succeeds when the ingress rate limiter is active and the system is in an
 * overloaded state. Verifies that resharding-specific internal appNames are properly exempted so
 * that internal resharding connections are not blocked by the rate limiter.
 *
 * Uses a two-shard ShardingTest with keyFile auth so that new unauthenticated connections are
 * exempt from rate limiting (via _isAuthorizationExempt), allowing the hello handshake to set
 * client metadata before any rate limiting is applied.
 *
 * @tags: [requires_fcv_83]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    kInternalConnectionAppNameExemptions,
    kRateLimiterExemptAppName,
    kSlowestRefreshRateSecs,
} from "jstests/noPassthrough/admission/libs/ingress_request_rate_limiter_helper.js";

const kKeyFile = "jstests/libs/key1";
const kUser = "admin";
const kPass = "pwd";

// All appName prefixes that must be exempted for resharding to succeed under rate limiting.
const kExemptions = [kRateLimiterExemptAppName, ...kInternalConnectionAppNameExemptions];

// Direct mongod connections (shard primaries, config primary) can only authenticate as __system
// via the keyfile — the admin user created via mongos lives only in the config server's user
// database and is not visible to direct shard connections. authutil.asCluster handles this.

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
    // Open a fresh connection with the exempt appName. With auth enabled, new unauthenticated
    // connections are exempt (via _isAuthorizationExempt), so hello succeeds and sets client
    // metadata. asCluster then authenticates as __system, and the subsequent setParameter is
    // exempt via the kRateLimiterExemptAppName appName in the exemption list.
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

describe("resharding with ingress rate limiter active", function () {
    it("completes successfully despite overloaded rate limiter on shard nodes", function () {
        const st = new ShardingTest({
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

        // Create the admin user before any users exist (allowed without authentication from an
        // unauthenticated connection), then create an exempt authenticated connection for all
        // subsequent operations via the mongos.
        st.s.getDB("admin").createUser({user: kUser, pwd: kPass, roles: ["root"]});
        const exemptConn = new Mongo(`mongodb://${st.s.host}/?appName=${kRateLimiterExemptAppName}`);
        exemptConn.getDB("admin").auth(kUser, kPass);

        assert.commandWorked(exemptConn.adminCommand({enableSharding: "test"}));
        assert.commandWorked(exemptConn.adminCommand({shardCollection: "test.coll", key: {oldKey: 1}}));

        const kNumDocs = 10;
        const coll = exemptConn.getDB("test").coll;
        for (let i = 0; i < kNumDocs; i++) {
            assert.commandWorked(coll.insert({oldKey: i, newKey: i}));
        }

        // Enable a simulated overloaded rate limiter on all shard primaries and the config server.
        // Without the resharding appName exemptions the operation would fail.
        const rateLimitedNodes = [st.configRS.getPrimary(), st.rs0.getPrimary(), st.rs1.getPrimary()];
        rateLimitedNodes.forEach(enableOverloadedRateLimiter);

        try {
            assert.commandWorked(
                exemptConn.adminCommand({
                    reshardCollection: "test.coll",
                    key: {newKey: 1},
                    _presetReshardedChunks: [
                        {min: {newKey: MinKey}, max: {newKey: MaxKey}, recipientShardId: st.shard1.shardName},
                    ],
                }),
            );
        } finally {
            rateLimitedNodes.forEach((p) => disableRateLimiter(p.host));
        }

        // Verify all documents survived resharding with correct values.
        assert.eq(coll.countDocuments({}), kNumDocs, "document count changed after resharding");
        for (let i = 0; i < kNumDocs; i++) {
            assert.eq(coll.countDocuments({oldKey: i, newKey: i}), 1, {
                msg: "missing document after resharding",
                oldKey: i,
            });
        }

        st.stop();
    });
});
