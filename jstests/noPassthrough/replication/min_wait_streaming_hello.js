/**
 * Tests that the minWaitForStreamingHelloMillis server parameter enforces a minimum timeout
 * for pre-auth streamable hello commands.
 * @tags: [requires_replication, requires_fcv_83]
 */
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("minWaitForStreamingHelloMillis", function () {
    before(function () {
        this.replTest = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    minWaitForStreamingHelloMillis: 2000,
                    abortStreamingHelloWithSmallTimeout: false,
                },
            },
        });
        this.replTest.startSet();
        this.replTest.initiate();
        this.primary = this.replTest.getPrimary();
        this.db = this.primary.getDB("admin");
    });

    after(function () {
        this.replTest.stopSet();
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
        assert.gte(
            elapsed,
            1800,
            "Expected command to wait at least 1800ms due to clamping, " + "but it completed in " + elapsed + "ms",
        );
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
        assert.gte(
            elapsed,
            1800,
            "Expected command to wait at least 1800ms, " + "but it completed in " + elapsed + "ms",
        );
    });

    it("aborts when abortStreamingHelloWithSmallTimeout is true", function () {
        // Enable abort mode.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                abortStreamingHelloWithSmallTimeout: true,
            }),
        );

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

        // Restore abort mode.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                abortStreamingHelloWithSmallTimeout: false,
            }),
        );
    });

    it("can update minWaitForStreamingHelloMillis at runtime", function () {
        // Set a lower minimum.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                minWaitForStreamingHelloMillis: 500,
            }),
        );

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
        assert.gte(
            elapsed,
            400,
            "Expected command to wait at least 400ms due to clamping, " + "but it completed in " + elapsed + "ms",
        );

        // Restore original value.
        assert.commandWorked(
            this.db.adminCommand({
                setParameter: 1,
                minWaitForStreamingHelloMillis: 2000,
            }),
        );
    });
});
