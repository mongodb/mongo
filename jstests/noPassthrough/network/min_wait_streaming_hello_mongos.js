/**
 * Tests that the minWaitForStreamingHelloMillis server parameter enforces a minimum timeout
 * for pre-auth streamable hello commands on mongos.
 *
 * mongos serves streamable hello from its own implementation (cluster_hello_cmd.cpp, backed by
 * MongosTopologyCoordinator) rather than the mongod one, so it needs coverage independent of
 * jstests/noPassthrough/replication/min_wait_streaming_hello.js.
 *
 * This test needs a binary that carries the mongos-side fix rather than a particular FCV, so it is
 * denylisted from multiversion suites via etc/backports_required_for_multiversion_tests.yml instead
 * of being tagged with requires_fcv_*.
 * @tags: [requires_sharding]
 */
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("minWaitForStreamingHelloMillis on mongos", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                auth: "",
                mongosOptions: {
                    setParameter: {
                        minWaitForStreamingHelloMillis: 2000,
                        abortStreamingHelloWithSmallTimeout: false,
                    },
                },
            },
        });
        this.db = this.st.s0.getDB("admin");

        // Create the admin user via the localhost exception. this.db itself is never
        // authenticated, so it keeps exercising the unauthenticated, pre-auth code path used by
        // the tests below even though the cluster now requires auth for other commands.
        this.db.createUser({user: "admin", pwd: "admin", roles: jsTest.adminUserRoles});
    });

    after(function () {
        this.st.stop();
    });

    it("clamps maxAwaitTimeMS to minimum when below threshold for unauthenticated client", function () {
        // Get the current topology version.
        const res = assert.commandWorked(this.db.runCommand({hello: 1}));
        const topologyVersion = res.topologyVersion;

        // An awaitable hello with maxAwaitTimeMS: 0 should be clamped to the minimum (2000ms),
        // so it should take at least ~2000ms to return when topology doesn't change.
        const start = new Date();
        assert.commandWorked(
            this.db.runCommand({
                hello: 1,
                topologyVersion: topologyVersion,
                maxAwaitTimeMS: 0,
            }),
        );
        const elapsed = new Date() - start;
        assert.gte(elapsed, 1800, "Expected command to wait at least 1800ms due to clamping", {
            elapsed,
        });
    });

    it("allows maxAwaitTimeMS at or above the minimum without clamping", function () {
        const res = assert.commandWorked(this.db.runCommand({hello: 1}));
        const topologyVersion = res.topologyVersion;

        // An awaitable hello with maxAwaitTimeMS >= minWaitForStreamingHelloMillis should not
        // be clamped. Use the exact minimum value.
        const start = new Date();
        assert.commandWorked(
            this.db.runCommand({
                hello: 1,
                topologyVersion: topologyVersion,
                maxAwaitTimeMS: 2000,
            }),
        );
        const elapsed = new Date() - start;
        // Should take approximately 2000ms (the provided value).
        assert.gte(elapsed, 1800, "Expected command to wait at least 1800ms", {elapsed});
    });

    it("aborts when abortStreamingHelloWithSmallTimeout is true", function () {
        // Enable abort mode.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                abortStreamingHelloWithSmallTimeout: true,
            }),
        );

        try {
            const res = assert.commandWorked(this.db.runCommand({hello: 1}));
            const topologyVersion = res.topologyVersion;

            // An awaitable hello with maxAwaitTimeMS below minimum should fail.
            assert.commandFailedWithCode(
                this.db.runCommand({
                    hello: 1,
                    topologyVersion: topologyVersion,
                    maxAwaitTimeMS: 0,
                }),
                ErrorCodes.InvalidOptions,
            );
        } finally {
            // Restore abort mode.
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    abortStreamingHelloWithSmallTimeout: false,
                }),
            );
        }
    });

    it("can update minWaitForStreamingHelloMillis at runtime", function () {
        // Set a lower minimum.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                minWaitForStreamingHelloMillis: 500,
            }),
        );

        try {
            const res = assert.commandWorked(this.db.runCommand({hello: 1}));
            const topologyVersion = res.topologyVersion;

            // With a minimum of 500ms, a maxAwaitTimeMS of 0 should be clamped to 500ms.
            const start = new Date();
            assert.commandWorked(
                this.db.runCommand({
                    hello: 1,
                    topologyVersion: topologyVersion,
                    maxAwaitTimeMS: 0,
                }),
            );
            const elapsed = new Date() - start;
            assert.gte(elapsed, 400, "Expected command to wait at least 400ms due to clamping", {
                elapsed,
            });
        } finally {
            // Restore original value.
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    minWaitForStreamingHelloMillis: 2000,
                }),
            );
        }
    });

    it("does not clamp maxAwaitTimeMS for an authenticated client", function () {
        // Use a separate, dedicated connection so authenticating it does not affect this.db,
        // which the other test cases above rely on staying unauthenticated.
        const authenticatedConn = new Mongo(this.st.s0.host);
        const authenticatedDb = authenticatedConn.getDB("admin");
        assert(authenticatedDb.auth("admin", "admin"));

        const res = assert.commandWorked(authenticatedDb.runCommand({hello: 1}));
        const topologyVersion = res.topologyVersion;

        // An authenticated client's maxAwaitTimeMS should never be clamped, even though 0ms is
        // far below minWaitForStreamingHelloMillis (2000ms), so this should return quickly.
        const start = new Date();
        assert.commandWorked(
            authenticatedDb.runCommand({
                hello: 1,
                topologyVersion: topologyVersion,
                maxAwaitTimeMS: 0,
            }),
        );
        const elapsed = new Date() - start;
        assert.lt(elapsed, 1800, "Expected an authenticated client's hello to return quickly, not be clamped", {
            elapsed,
        });

        authenticatedConn.close();
    });
});
