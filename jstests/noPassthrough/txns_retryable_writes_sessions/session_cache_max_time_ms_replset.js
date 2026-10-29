/**
 * Tests that with logicalSessionCacheJobTimeoutEnabled, a secondary's session cache refresh
 * appends maxTimeMS (90% of logicalSessionRefreshMillis) to the update batches it sends to the
 * primary, so a stalled update aborts with MaxTimeMSExpired instead of hanging.
 *
 * Replica-set coverage of the maxTimeMS half of SERVER-127346; see SERVER-128093.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

TestData.disableImplicitSessions = true;

// The injected maxTimeMS is 90% of logicalSessionRefreshMillis.
const kRefreshMillis = 5000;
const kExpectedTimeoutMs = (kRefreshMillis * 9) / 10;
// The aborted refresh must return well within this bound.
const kUpperBoundMs = 30 * 1000;

describe("secondary session refresh withRefreshTimeout deadline fires on primary", function () {
    let rst;
    let primary;
    let secondary;

    before(function () {
        rst = new ReplSetTest({
            nodes: 2,
            nodeOptions: {
                setParameter: {
                    enableTestCommands: 1,
                    logicalSessionCacheJobTimeoutEnabled: true,
                    // Prevent background periodic refresh from draining _activeSessions.
                    disableLogicalSessionCacheRefresh: true,
                    // Small refresh interval so withRefreshTimeout computes a short deadline.
                    logicalSessionRefreshMillis: kRefreshMillis,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();

        // Create config.system.sessions on the primary, then replicate to secondary.
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        rst.awaitReplication();
    });

    after(function () {
        rst.stopSet();
    });

    it("withRefreshTimeout deadline fires when primary stalls past it", function () {
        // Seed the secondary's _activeSessions so refresh has work to flush.
        for (let i = 0; i < 25; i++) {
            assert.commandWorked(
                secondary.getDB("admin").runCommand({ping: 1, lsid: {id: UUID()}}),
            );
        }

        // Hang the update on the primary. shouldCheckForInterrupt releases the hang with
        // MaxTimeMSExpired once the injected maxTimeMS deadline fires.
        const fp = configureFailPoint(primary, "hangDuringBatchUpdate", {
            nss: "config.system.sessions",
            shouldCheckForInterrupt: true,
        });

        try {
            const start = Date.now();
            // No maxTimeMS on the command; the deadline comes from withRefreshTimeout.
            const res = secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
            const elapsed = Date.now() - start;
            jsTest.log.info("refreshLogicalSessionCacheNow under stall", {res, elapsedMs: elapsed});

            assert.commandFailedWithCode(
                res,
                ErrorCodes.MaxTimeMSExpired,
                "expected MaxTimeMSExpired when primary stalls past the withRefreshTimeout deadline",
            );
            assert.lt(
                elapsed,
                kUpperBoundMs,
                "command should return at the withRefreshTimeout deadline",
                {
                    elapsed,
                    kUpperBoundMs,
                    kExpectedTimeoutMs,
                },
            );
        } finally {
            fp.off();
        }
    });
});
