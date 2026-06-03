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

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Disable implicit sessions so we can control session creation.
TestData.disableImplicitSessions = true;

describe("Session cache operations use kExempt admission priority", function () {
    let rst;
    let primary;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    enableTestCommands: 1,
                    logicalSessionCacheJobTimeoutEnabled: true,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
    });

    after(function () {
        rst.stopSet();
    });

    it("should complete session refresh even when write ticket concurrency is zero", function () {
        // Create a session so refresh has work to do.
        assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));

        const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

        const origWriteTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentWriteTransactions: 1}),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: 0}));

        try {
            // Session refresh must complete because it uses kExempt admission priority and
            // bypasses the ticket queue entirely.
            assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        } finally {
            // Restore write ticket concurrency to avoid affecting subsequent tests.
            assert.commandWorked(
                primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: origWriteTickets}),
            );
        }

        // Confirm the session upsert actually reached config.system.sessions.  If
        // _activeSessions was empty the refresh would be a no-op and the test would prove
        // nothing about write-ticket exemption.
        const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
        assert.gt(sessionDocsAfter, sessionDocsBefore, "expected session to be upserted into config.system.sessions", {
            sessionDocsBefore,
            sessionDocsAfter,
        });
    });

    it("should complete session reap even when write ticket concurrency is zero", function () {
        // Seed a session and refresh it into config.system.sessions so the reaper has
        // real collection state to scan — otherwise the reap is a trivial no-op.
        assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        const reaperJobsBefore = primary.adminCommand({serverStatus: 1}).logicalSessionRecordCache
            .transactionReaperJobCount;

        const origWriteTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentWriteTransactions: 1}),
        ).executionControlConcurrentWriteTransactions;

        // Set write concurrency to 0 to block all non-exempt write operations.
        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: 0}));

        try {
            // Session reap must complete because it uses kExempt admission priority and
            // bypasses the ticket queue entirely.
            assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                primary.adminCommand({setParameter: 1, executionControlConcurrentWriteTransactions: origWriteTickets}),
            );
        }

        // Confirm the reaper actually ran a job cycle — if it returned without executing
        // due to an error the job count would be unchanged.
        const reaperJobsAfter = primary.adminCommand({serverStatus: 1}).logicalSessionRecordCache
            .transactionReaperJobCount;
        assert.gt(reaperJobsAfter, reaperJobsBefore, "expected the reaper to have run at least one job cycle", {
            reaperJobsBefore,
            reaperJobsAfter,
        });
    });

    it("should increment the exempt write admission counter when refreshing sessions", function () {
        // Create a session so refresh has an upsert to send to config.system.sessions.
        assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));

        const execStatsBefore = primary.adminCommand({serverStatus: 1}).queues.execution;
        const exemptWritesBefore = execStatsBefore.write.exempt.startedProcessing;

        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        const execStatsAfter = primary.adminCommand({serverStatus: 1}).queues.execution;
        const exemptWritesAfter = execStatsAfter.write.exempt.startedProcessing;

        // The session upsert (and any session delete) must have run as kExempt, incrementing
        // the exempt write counter.  A regression to kNormal would leave the counter unchanged.
        assert.gt(
            exemptWritesAfter,
            exemptWritesBefore,
            "expected exempt write counter to increase after session refresh",
            {exemptWritesBefore, exemptWritesAfter},
        );
    });

    it("should increment the exempt read admission counter when refreshing sessions", function () {
        // Create and persist a session so findRemovedSessions has a cursor session to check.
        assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));
        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        const execStatsBefore = primary.adminCommand({serverStatus: 1}).queues.execution;
        const exemptReadsBefore = execStatsBefore.read.exempt.startedProcessing;

        assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));

        const execStatsAfter = primary.adminCommand({serverStatus: 1}).queues.execution;
        const exemptReadsAfter = execStatsAfter.read.exempt.startedProcessing;

        // findRemovedSessions issues a kExempt read against config.system.sessions.
        // A regression to kNormal would leave the counter unchanged.
        assert.gt(
            exemptReadsAfter,
            exemptReadsBefore,
            "expected exempt read counter to increase after session refresh",
            {exemptReadsBefore, exemptReadsAfter},
        );
    });

    it("should complete session refresh even when read ticket concurrency is zero", function () {
        // Seed a session so refresh has a real upsert to perform and findRemovedSessions
        // has state to read — without this the refresh is a no-op for both paths.
        assert.commandWorked(primary.getDB("admin").runCommand({startSession: 1}));

        const sessionDocsBefore = primary.getDB("config").system.sessions.countDocuments({});

        const origReadTickets = assert.commandWorked(
            primary.adminCommand({getParameter: 1, executionControlConcurrentReadTransactions: 1}),
        ).executionControlConcurrentReadTransactions;

        // Set read concurrency to 0 to block all non-exempt read operations.
        assert.commandWorked(primary.adminCommand({setParameter: 1, executionControlConcurrentReadTransactions: 0}));

        try {
            // Session refresh reads config.system.sessions (findRemovedSessions); this must
            // complete because it uses kExempt admission priority.
            assert.commandWorked(primary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1}));
        } finally {
            assert.commandWorked(
                primary.adminCommand({setParameter: 1, executionControlConcurrentReadTransactions: origReadTickets}),
            );
        }

        // Confirm the session upsert (refreshSessions write) ran during the refresh.  The
        // write uses write tickets which are not restricted here, so a count increase proves
        // the refresh executed its full pipeline rather than bailing out early.
        const sessionDocsAfter = primary.getDB("config").system.sessions.countDocuments({});
        assert.gt(sessionDocsAfter, sessionDocsBefore, "expected session to be upserted into config.system.sessions", {
            sessionDocsBefore,
            sessionDocsAfter,
        });
    });
});
