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

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Disable implicit sessions so we control exactly when sessions are created.
TestData.disableImplicitSessions = true;

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            enableTestCommands: 1,
            logicalSessionCacheJobTimeoutEnabled: true,
            // Use a very long refresh interval so the background periodic refresh does not race
            // with our explicit refreshLogicalSessionCacheNow calls.
            logicalSessionRefreshMillis: 24 * 60 * 60 * 1000,
        },
    },
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const secondary = rst.getSecondary();

// The primary's first refresh creates config.system.sessions.  Without this, the secondary's
// setupSessionsCollection (remote path) checks for the collection locally via DBDirectClient
// and throws NamespaceNotFound.
assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
rst.awaitReplication();

// --- Test 1: session refresh (update) from secondary with primary write tickets = 0 ---
{
    assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));

    const cacheStats = secondary.getDB("admin").serverStatus().logicalSessionRecordCache;
    assert.gte(cacheStats.activeSessionsCount,
               1,
               "expected at least one session in the secondary's cache",
               {cacheStats});

    const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storageEngineConcurrentWriteTransactions: 0}));

    try {
        assert.commandWorked(
            secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    } finally {
        assert.commandWorked(primary.adminCommand({
            setParameter: 1,
            storageEngineConcurrentWriteTransactions: 128,
        }));
    }

    const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
    assert.gt(sessionDocsAfter,
              sessionDocsBefore,
              "expected the secondary's session to be upserted into config.system.sessions",
              {sessionDocsBefore, sessionDocsAfter});
}

// --- Test 2: session delete (removeRecords) from secondary with primary write tickets = 0 ---
{
    const startRes = assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
    assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    assert.commandWorked(secondary.getDB("admin").runCommand({endSessions: [startRes.id]}));

    const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});
    assert.gte(sessionDocsBefore, 1, "expected at least one session document before delete");

    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storageEngineConcurrentWriteTransactions: 0}));

    try {
        assert.commandWorked(
            secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    } finally {
        assert.commandWorked(primary.adminCommand({
            setParameter: 1,
            storageEngineConcurrentWriteTransactions: 128,
        }));
    }

    const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
    assert.lt(sessionDocsAfter,
              sessionDocsBefore,
              "expected the ended session to be deleted from config.system.sessions",
              {sessionDocsBefore, sessionDocsAfter});
}

// --- Test 3: findRemovedSessions (read) from secondary with primary read tickets = 0 ---
{
    assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
    assert.commandWorked(secondary.getDB("admin").runCommand({startSession: 1}));
    assert.commandWorked(secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

    const cursorSession = secondary.startSession();
    const cursor =
        cursorSession.getDatabase("config").getCollection("system.sessions").find({}).batchSize(1);
    assert(cursor.hasNext(), "expected at least one session document so the cursor stays open");

    const openCursorsBefore = secondary.getDB("admin").serverStatus().metrics.cursor.open.total;
    assert.gte(openCursorsBefore,
               1,
               "expected an open server-side cursor so findRemovedSessions has work to do");

    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storageEngineConcurrentReadTransactions: 0}));

    try {
        assert.commandWorked(
            secondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    } finally {
        assert.commandWorked(primary.adminCommand({
            setParameter: 1,
            storageEngineConcurrentReadTransactions: 128,
        }));
        cursor.close();
        cursorSession.endSession();
    }
}

rst.stopSet();
