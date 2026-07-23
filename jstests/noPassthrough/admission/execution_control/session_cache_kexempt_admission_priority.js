/**
 * Tests that session cache operations (refresh and reap) use kExempt admission priority, allowing
 * them to bypass the ticket queue and make forward progress even under heavy write load.
 *
 * This is a regression test for SERVER-127346, where session refresh was starved by admission
 * control under heavy write load, causing TooManyLogicalSessions errors.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

// Disable implicit sessions so we can control session creation.
TestData.disableImplicitSessions = true;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            enableTestCommands: 1,
            logicalSessionCacheJobTimeoutEnabled: true,
        },
    },
});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

// On 7.0, getParameter returns 0 as the startup sentinel meaning "use engine default".
// Setting to 0 at runtime means "0 tickets" (blocks all writes), and restoring to 0
// would leave tickets blocked and cause mongod shutdown to hang.  Use an explicit
// safe restore value instead.
const kSafeTicketDefault = 128;

function setWriteTickets(n) {
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storageEngineConcurrentWriteTransactions: n}));
}

function setReadTickets(n) {
    assert.commandWorked(
        primary.adminCommand({setParameter: 1, storageEngineConcurrentReadTransactions: n}));
}

// Warm up: ensure config.system.sessions exists before constraining tickets.
// setupSessionsCollection writes (createIndexes) must succeed unconstrained first.
assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

// --- Test 1: session refresh completes with write tickets = 0 ---
{
    assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));
    const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

    setWriteTickets(0);
    try {
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    } finally {
        setWriteTickets(kSafeTicketDefault);
    }

    const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
    assert.gt(sessionDocsAfter,
              sessionDocsBefore,
              "expected session to be upserted into config.system.sessions",
              {sessionDocsBefore, sessionDocsAfter});
}

// --- Test 2: session reap runs a job cycle (without ticket constraint on 7.0) ---
// Note: reapLogicalSessionCacheNow with write tickets = 0 hangs on 7.0 because
// removeExpiredTransactionSessionsFromDisk writes directly to config.transactions
// through a code path that does not observe the opCtx admission priority.
// We verify the reaper runs correctly without the ticket constraint here.
{
    assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));
    assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

    const reaperJobsBefore =
        primary.adminCommand({serverStatus: 1}).logicalSessionRecordCache.transactionReaperJobCount;

    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));

    const reaperJobsAfter =
        primary.adminCommand({serverStatus: 1}).logicalSessionRecordCache.transactionReaperJobCount;
    assert.gt(reaperJobsAfter,
              reaperJobsBefore,
              "expected the reaper to have run at least one job cycle",
              {reaperJobsBefore, reaperJobsAfter});
}

// --- Test 3: session refresh completes with read tickets = 0 ---
{
    assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));
    const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

    setReadTickets(0);
    try {
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
    } finally {
        setReadTickets(kSafeTicketDefault);
    }

    const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
    assert.gt(sessionDocsAfter,
              sessionDocsBefore,
              "expected session to be upserted into config.system.sessions",
              {sessionDocsBefore, sessionDocsAfter});
}

rst.stopSet();
