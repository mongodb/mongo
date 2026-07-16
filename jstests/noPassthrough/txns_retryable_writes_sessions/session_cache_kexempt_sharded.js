/**
 * Tests that session cache operations (refresh and reap) use kExempt admission priority on a
 * sharded cluster: both for a shard's local refresh and for refresh traffic that mongos sends
 * to the shard as internal-client commands against config.system.sessions.
 *
 * Sharded coverage for SERVER-127346, which fixed and tested replica sets only.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Disable implicit sessions so we control exactly when sessions are created.
TestData.disableImplicitSessions = true;

describe("Session cache kExempt admission priority on sharded clusters", function () {
    let st;
    let mongos;
    let shardPrimary;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {
                nodes: 1,
                setParameter: {
                    enableTestCommands: 1,
                    logicalSessionCacheJobTimeoutEnabled: true,
                },
            },
            other: {
                mongosOptions: {
                    setParameter: {
                        enableTestCommands: 1,
                        logicalSessionCacheJobTimeoutEnabled: true,
                    },
                },
            },
        });
        mongos = st.s0;
        shardPrimary = st.rs0.getPrimary();
    });

    after(function () {
        st.stop();
    });

    it("should complete shard-local session refresh when write ticket concurrency is zero", function () {
        // Create a session directly on the shard so its local refresh has an upsert to do.
        assert.commandWorked(shardPrimary.getDB("admin").runCommand({startSession: 1}));

        const sessionDocsBefore = shardPrimary.getDB("config").system.sessions.countDocuments({});

        const origWriteTickets = assert.commandWorked(
            shardPrimary.adminCommand({
                getParameter: 1,
                executionControlConcurrentWriteTransactions: 1,
            }),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                executionControlConcurrentWriteTransactions: 0,
            }),
        );

        try {
            // The shard's own refresh must complete because it uses kExempt admission priority.
            assert.commandWorked(
                shardPrimary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}),
            );
        } finally {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm the session upsert actually reached config.system.sessions; otherwise the
        // refresh was a no-op and the test proves nothing about write-ticket exemption.
        const sessionDocsAfter = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.gt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected session to be upserted into config.system.sessions",
            {
                sessionDocsBefore,
                sessionDocsAfter,
            },
        );
    });

    it("should complete mongos session refresh when shard write ticket concurrency is zero", function () {
        // Create a session on mongos so its refresh sends an update to the shard.
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));

        const sessionDocsBefore = shardPrimary.getDB("config").system.sessions.countDocuments({});

        const origWriteTickets = assert.commandWorked(
            shardPrimary.adminCommand({
                getParameter: 1,
                executionControlConcurrentWriteTransactions: 1,
            }),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                executionControlConcurrentWriteTransactions: 0,
            }),
        );

        try {
            // The update arrives at the shard as internal-client traffic against
            // config.system.sessions, so write_commands applies kExempt on the receiving opCtx
            // and the write bypasses the ticket queue.  Without kExempt this hangs.
            assert.commandWorked(
                mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}),
            );
        } finally {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm a write actually reached the shard and completed.
        const sessionDocsAfter = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.gt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected the mongos session to be upserted into config.system.sessions",
            {sessionDocsBefore, sessionDocsAfter},
        );
    });

    it("should complete mongos session delete when shard write ticket concurrency is zero", function () {
        // Create a session on mongos and refresh it so a document exists in
        // config.system.sessions to delete.
        const startRes = assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        // Explicitly end the session so the next refresh calls removeRecords with a non-empty
        // delete batch, which mongos forwards to the shard as an internal-client delete.
        assert.commandWorked(mongos.getDB("admin").runCommand({endSessions: [startRes.id]}));

        const sessionDocsBefore = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.gte(
            sessionDocsBefore,
            1,
            "expected at least one session document to exist before delete",
            {sessionDocsBefore},
        );

        const origWriteTickets = assert.commandWorked(
            shardPrimary.adminCommand({
                getParameter: 1,
                executionControlConcurrentWriteTransactions: 1,
            }),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                executionControlConcurrentWriteTransactions: 0,
            }),
        );

        try {
            // The delete arrives at the shard as internal-client traffic against
            // config.system.sessions, so write_commands applies kExempt on the receiving opCtx.
            // This exercises the CmdDelete admission-priority guard, which is a separate code path
            // from the CmdUpdate guard the refresh test above covers.  Without kExempt this hangs.
            assert.commandWorked(
                mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}),
            );
        } finally {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm the delete actually reached the shard and completed.  A false positive would
        // show an unchanged count (empty ending set -> no delete sent).
        const sessionDocsAfter = shardPrimary.getDB("config").system.sessions.countDocuments({});
        assert.lt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected the ended session to be deleted from config.system.sessions",
            {sessionDocsBefore, sessionDocsAfter},
        );
    });

    it("should complete mongos findRemovedSessions when shard read ticket concurrency is zero", function () {
        // Seed two sessions so config.system.sessions has at least two documents.
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        // Open a cursor through mongos within a shell session. With batchSize(1) and 2+ docs the
        // cursor stays open, so its lsid appears in getOpenCursorSessions and the next refresh
        // sends a find to the shard. Without an open cursor no find would be sent at all.
        const cursorSession = mongos.startSession();
        const cursor = cursorSession
            .getDatabase("config")
            .getCollection("system.sessions")
            .find({})
            .batchSize(1);
        assert(cursor.hasNext(), "expected at least one session document so the cursor stays open");

        const origReadTickets = assert.commandWorked(
            shardPrimary.adminCommand({
                getParameter: 1,
                executionControlConcurrentReadTransactions: 1,
            }),
        ).executionControlConcurrentReadTransactions;

        // Set read concurrency to 0 to block all non-exempt read operations.
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                executionControlConcurrentReadTransactions: 0,
            }),
        );

        try {
            // The find arrives at the shard as internal-client traffic against
            // config.system.sessions, so find_cmd applies kExempt on the receiving opCtx and the
            // read bypasses the ticket queue.  Without kExempt this hangs.
            assert.commandWorked(
                mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}),
            );
        } finally {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: origReadTickets,
                }),
            );
            cursor.close();
            cursorSession.endSession();
        }
    });

    it("should increment the shard's exempt write counter after a mongos session refresh", function () {
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));

        const exemptWritesBefore = shardPrimary.adminCommand({serverStatus: 1}).queues.execution
            .write.exempt.startedProcessing;

        assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        // The session upsert forwarded by mongos must have run as kExempt on the shard.  A
        // regression to kNormal would leave the counter unchanged.
        const exemptWritesAfter = shardPrimary.adminCommand({serverStatus: 1}).queues.execution
            .write.exempt.startedProcessing;
        assert.gt(
            exemptWritesAfter,
            exemptWritesBefore,
            "expected the shard's exempt write counter to increase after a mongos session refresh",
            {exemptWritesBefore, exemptWritesAfter},
        );
    });

    it("should complete shard session reap when write ticket concurrency is zero", function () {
        // Seed a session and refresh it into config.system.sessions so the reaper has real
        // collection state to scan.
        assert.commandWorked(shardPrimary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(
            shardPrimary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}),
        );

        const reaperJobsBefore = shardPrimary.adminCommand({serverStatus: 1})
            .logicalSessionRecordCache.transactionReaperJobCount;

        const origWriteTickets = assert.commandWorked(
            shardPrimary.adminCommand({
                getParameter: 1,
                executionControlConcurrentWriteTransactions: 1,
            }),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(
            shardPrimary.adminCommand({
                setParameter: 1,
                executionControlConcurrentWriteTransactions: 0,
            }),
        );

        try {
            // Session reap must complete because it uses kExempt admission priority.
            assert.commandWorked(shardPrimary.adminCommand({reapLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                shardPrimary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm the reaper actually ran a job cycle.
        const reaperJobsAfter = shardPrimary.adminCommand({serverStatus: 1})
            .logicalSessionRecordCache.transactionReaperJobCount;
        assert.gt(
            reaperJobsAfter,
            reaperJobsBefore,
            "expected the reaper to have run at least one job cycle",
            {
                reaperJobsBefore,
                reaperJobsAfter,
            },
        );
    });
});
