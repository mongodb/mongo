/**
 * Tests that when a secondary's session cache forwards operations to the primary via
 * SessionsCollectionRS (the remote execution path in _dispatch), those operations arrive
 * at the primary with kExempt admission priority so they can make forward progress even
 * when all write tickets are exhausted.
 *
 * The fix has two parts:
 *   1. LogicalSessionCacheImpl::_reap and _refresh set kExempt on their opCtx via
 *      ScopedAdmissionPriority.  For the local (DBDirectClient) path this priority
 *      propagates directly because the same opCtx is reused.
 *   2. For the remote path (secondary forwarding to primary), the write_commands update
 *      and delete handlers apply kExempt on the receiving opCtx when the namespace is
 *      config.system.sessions and the client is internal.
 *
 * Without the fix, the forwarded update queues on the primary waiting for a write ticket
 * that is never granted, and the secondary blocks on the response indefinitely.
 *
 * This is a regression test for SERVER-127346.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Disable implicit sessions so we control exactly when sessions are created.
TestData.disableImplicitSessions = true;

describe("Secondary session cache remote dispatch uses kExempt admission priority", function () {
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
                    // Use a very long refresh interval so the background periodic refresh
                    // does not race with our explicit refreshLogicalSessionCacheNow calls
                    // and consume sessions from _activeSessions before we trigger them.
                    logicalSessionRefreshMillis: 24 * 60 * 60 * 1000,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();

        // The primary's first refresh creates config.system.sessions.  Without this, the
        // secondary's setupSessionsCollection (remote path) checks for the collection
        // locally via DBDirectClient and throws NamespaceNotFound.
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        rst.awaitReplication();
    });

    after(function () {
        rst.stopSet();
    });

    /**
     * Functional verification: session refresh from a secondary completes even when the
     * primary has zero write tickets available.
     *
     * Without the fix the forwarded update queues on the primary waiting for a write ticket
     * that is never granted and the secondary blocks indefinitely on the response.  With the
     * fix, write_commands applies kExempt on the receiving opCtx for internal-client writes
     * to config.system.sessions, so the update bypasses the ticket queue and completes.
     */
    it("should complete session refresh on secondary even when primary write tickets are zero", function () {
        // Seed one session on the secondary.  startSession adds a record to the
        // secondary's LogicalSessionCache._activeSessions, giving the refresh work to do
        // so that refreshSessions actually forwards an update to the primary.
        assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));

        // Confirm the session is actually in the secondary's cache before we proceed.
        // If this is zero the refresh would be a no-op and the test proves nothing.
        const cacheStats = secondary.getDB("admin").serverStatus().logicalSessionRecordCache;
        assert.gte(cacheStats.activeSessionsCount, 1, "expected at least one session in the secondary's cache", {
            cacheStats,
        });

        // Snapshot the session collection size so we can verify a write was forwarded.
        const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

        const origWriteTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentWriteTransactions: 1}),
        ).executionControlConcurrentWriteTransactions;

        // Block all non-exempt writes on the primary.
        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: 0}));

        try {
            // The session upsert forwarded from the secondary to the primary must carry
            // kExempt admission priority so it bypasses the ticket queue and returns.
            // Without kExempt this hangs: the primary queues the write waiting for a
            // ticket that is never granted, and the secondary blocks on the response.
            assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                primary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm that a write actually reached the primary and completed.  If the
        // refresh succeeded because _activeSessions was empty (no write needed) the
        // doc count would be unchanged and the test would be a false positive.
        const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
        assert.gt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected the secondary's session to be upserted into config.system.sessions",
            {sessionDocsBefore, sessionDocsAfter},
        );
    });

    /**
     * Tests the delete path: removeRecords forwards a delete to the primary when the secondary
     * has explicitly ended sessions (_endingSessions is non-empty).  write_commands applies
     * kExempt on the receiving opCtx for internal-client deletes to config.system.sessions.
     */
    it("should complete session delete on secondary even when primary write tickets are zero", function () {
        // Create a session and immediately refresh it so a document exists in
        // config.system.sessions to delete.
        const startRes = assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        // Explicitly end the session.  This populates the secondary's _endingSessions so
        // that the next refresh calls removeRecords with a non-empty delete batch.
        assert.commandWorked(secondary.getDB("admin").runCommand({endSessions: [startRes.id]}));

        const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});
        assert.gte(sessionDocsBefore, 1, "expected at least one session document to exist before delete", {
            sessionDocsBefore,
        });

        const origWriteTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentWriteTransactions: 1}),
        ).executionControlConcurrentWriteTransactions;

        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: 0}));

        try {
            // The delete forwarded from the secondary to the primary must be kExempt so it
            // bypasses the ticket queue.  Without kExempt this hangs indefinitely.
            assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                primary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentWriteTransactions: origWriteTickets,
                }),
            );
        }

        // Confirm the delete actually reached the primary.  A false positive would show an
        // unchanged count (empty _endingSessions → no delete sent).
        const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
        assert.lt(
            sessionDocsAfter,
            sessionDocsBefore,
            "expected the ended session to be deleted from config.system.sessions",
            {sessionDocsBefore, sessionDocsAfter},
        );
    });

    /**
     * Tests the find path: findRemovedSessions forwards a find to the primary when the secondary
     * has open cursors associated with sessions (openCursorSessions is non-empty).  find_cmd
     * applies kExempt on the receiving opCtx for internal-client reads from
     * config.system.sessions.
     */
    it("should complete findRemovedSessions on secondary even when primary read tickets are zero", function () {
        // Seed two sessions so config.system.sessions has at least two documents.
        assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        // Open a cursor on config.system.sessions within a shell session.  With batchSize(1)
        // and 2+ docs, one document remains after the initial batch so the server keeps the
        // cursor open in the CursorManager, associated with the session's lsid.  That lsid
        // will appear in getOpenCursorSessions, causing findRemovedSessions to send a find
        // to the primary.
        const cursorSession = secondary.startSession();
        const cursor = cursorSession.getDatabase("config").getCollection("system.sessions").find({}).batchSize(1);
        assert(cursor.hasNext(), "expected at least one session document so the cursor stays open");

        // Verify the cursor is registered as open on the secondary.
        const openCursorsBefore = secondary.getDB("admin").serverStatus().metrics.cursor.open.total;
        assert.gte(openCursorsBefore, 1, "expected an open server-side cursor so findRemovedSessions has work to do", {
            openCursorsBefore,
        });

        const origReadTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentReadTransactions: 1}),
        ).executionControlConcurrentReadTransactions;

        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentReadTransactions: 0}));

        try {
            // findRemovedSessions forwards a find to the primary to check which open-cursor
            // sessions are no longer in config.system.sessions.  The find must be kExempt
            // to bypass the read ticket queue.  Without kExempt this hangs indefinitely.
            assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                primary.adminCommand({
                    setParameter: 1,
                    executionControlConcurrentReadTransactions: origReadTickets,
                }),
            );
            cursor.close();
            cursorSession.endSession();
        }
    });
});
