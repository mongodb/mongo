/**
 * Tests that with logicalSessionCacheJobTimeoutEnabled, the background session cache refresh on
 * mongos applies a deadline (90% of logicalSessionRefreshMillis) that propagates to the shard, so
 * a stalled write to config.system.sessions aborts at the deadline instead of wedging the refresh
 * job forever.
 *
 * Sharded coverage of the maxTimeMS half of SERVER-127346; see SERVER-128093.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Disable implicit sessions so we control exactly when sessions are created.
TestData.disableImplicitSessions = true;

// Short refresh interval so the background job cycles quickly; the job deadline is 90% of this.
const kRefreshMillis = 2000;

describe("mongos background session refresh deadline fires when the shard stalls", function () {
    let st;
    let mongos;
    let shardPrimary;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1, setParameter: {enableTestCommands: 1}},
            other: {
                mongosOptions: {
                    setParameter: {
                        enableTestCommands: 1,
                        logicalSessionCacheJobTimeoutEnabled: true,
                        logicalSessionRefreshMillis: kRefreshMillis,
                        // The shell disables the background refresh job by default; this test
                        // is specifically about the background job's deadline.
                        disableLogicalSessionCacheRefresh: false,
                    },
                },
            },
        });
        mongos = st.s0;
        shardPrimary = st.rs0.getPrimary();

        // Make config.system.sessions exist.
        assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    });

    after(function () {
        st.stop();
    });

    it("background refresh keeps cycling while shard updates are stalled", function () {
        const getJobCount = () =>
            mongos.getDB("admin").serverStatus().logicalSessionRecordCache
                .sessionsCollectionJobCount;

        // Hang every update on config.system.sessions on the shard.  With
        // shouldCheckForInterrupt the hang is released when the operation's deadline fires;
        // without a deadline it hangs until the failpoint is disabled.
        const fp = configureFailPoint(shardPrimary, "hangDuringBatchUpdate", {
            nss: "config.system.sessions",
            shouldCheckForInterrupt: true,
        });

        try {
            // Create sessions on mongos so the next background refresh sends an update batch
            // to the shard.
            for (let i = 0; i < 10; i++) {
                assert.commandWorked(
                    mongos.getDB("admin").runCommand({ping: 1, lsid: {id: UUID()}}),
                );
            }

            // Wait until a refresh update is actually hanging on the shard, proving the
            // background job flushed our sessions and is now stalled.
            fp.wait();

            // The hung job must abort at its deadline and later cycles must keep running.
            // Without the deadline the hung cycle never finishes, the periodic job never runs
            // again, and the count stays flat.
            const jobsBefore = getJobCount();
            assert.soon(
                () => getJobCount() >= jobsBefore + 2,
                "background refresh job did not keep cycling while the shard was stalled; " +
                    "the job deadline likely did not fire",
                60 * 1000,
            );
        } finally {
            fp.off();
        }

        // The cache must still work once the stall clears: a new session refreshes through to
        // the shard.
        const sessionDocsBefore = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        const sessionDocsAfter = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.gt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected session refresh to recover after the stall",
            {
                sessionDocsBefore,
                sessionDocsAfter,
            },
        );
    });
});
