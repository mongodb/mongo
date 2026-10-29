/**
 * Verifies serverStatus.transactions counts user-initiated transactions as external and
 * server-initiated ones as internal on an unsharded replica set.
 *
 * @tags: [uses_transactions]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("serverStatus.transactions internal/external classification", function () {
    const dbName = "test";
    const collName = "coll";

    let rst;
    let primary;
    let secondaries;

    before(function () {
        rst = new ReplSetTest({nodes: 3});
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        secondaries = rst.getSecondaries();

        assert.commandWorked(
            primary.getDB(dbName).createCollection(collName, {writeConcern: {w: "majority"}}),
        );
    });

    after(function () {
        rst.stopSet();
    });

    function getTransactionsSection(conn) {
        return assert.commandWorked(conn.adminCommand({serverStatus: 1})).transactions;
    }

    /**
     * Reads the transactions section from every node. Waits for the secondaries to catch up first,
     * so that their numbers reflect a fully-replicated state rather than a race.
     */
    function pollAllNodes(label) {
        rst.awaitReplication();
        const stats = {
            primary: getTransactionsSection(primary),
            secondaries: secondaries.map(getTransactionsSection),
        };
        jsTest.log.info(`serverStatus.transactions [${label}]`, {stats});
        return stats;
    }

    /**
     * Runs the three statements of the transaction body. Assumes the transaction has already been
     * started on 'sessionColl'.
     */
    function runThreeStatements(sessionColl, docId) {
        assert.commandWorked(sessionColl.insert({_id: docId, x: 1}));
        assert.commandWorked(sessionColl.update({_id: docId}, {$set: {x: 2}}));
        assert.commandWorked(sessionColl.remove({_id: docId}));
    }

    const countedFields = [
        "totalStarted",
        "totalStartedInternal",
        "totalStartedExternal",
        "totalCommitted",
        "totalCommittedInternal",
        "totalCommittedExternal",
        "totalAborted",
        "totalAbortedInternal",
        "totalAbortedExternal",
        "totalPrepared",
    ];

    /**
     * Asserts that the primary's counters moved by exactly 'expectedDeltas' (fields omitted from
     * 'expectedDeltas' must not move), that the split fields still sum to their totals, and that no
     * secondary counted anything: these transactions are unprepared, so secondaries replicate them
     * as a single applyOps entry and never open a transaction of their own.
     */
    function assertDeltas(prev, curr, expectedDeltas) {
        for (const field of countedFields) {
            const expected = expectedDeltas[field] ?? 0;
            assert.eq(
                expected,
                curr.primary[field] - prev.primary[field],
                `unexpected delta for ${field} on primary`,
                {prev, curr},
            );
        }

        for (const total of ["totalStarted", "totalCommitted", "totalAborted"]) {
            assert.eq(
                curr.primary[total],
                curr.primary[`${total}Internal`] + curr.primary[`${total}External`],
                `${total} split does not sum on primary`,
                {curr},
            );
        }

        assert.eq(0, curr.primary.currentOpen, "unexpected currentOpen on primary", {curr});

        curr.secondaries.forEach((secondaryStats, i) => {
            for (const field of countedFields) {
                assert.eq(
                    prev.secondaries[i][field],
                    secondaryStats[field],
                    `unexpected change to ${field} on secondary ${secondaries[i].host}`,
                    {prev, curr},
                );
            }
            assert.eq(0, secondaryStats.currentOpen, "unexpected currentOpen on secondary", {curr});
        });
    }

    it("counts a committed user transaction as external", function () {
        const prev = pollAllNodes("before committed user txn");

        const session = primary.startSession();
        session.startTransaction({writeConcern: {w: "majority"}});
        runThreeStatements(session.getDatabase(dbName).getCollection(collName), 1);
        assert.commandWorked(session.commitTransaction_forTesting());
        session.endSession();

        assertDeltas(prev, pollAllNodes("after committed user txn"), {
            totalStarted: 1,
            totalStartedExternal: 1,
            totalCommitted: 1,
            totalCommittedExternal: 1,
        });
    });

    it("counts an aborted user transaction as external", function () {
        const prev = pollAllNodes("before aborted user txn");

        const session = primary.startSession();
        session.startTransaction({writeConcern: {w: "majority"}});
        runThreeStatements(session.getDatabase(dbName).getCollection(collName), 2);
        assert.commandWorked(session.abortTransaction_forTesting());
        session.endSession();

        assertDeltas(prev, pollAllNodes("after aborted user txn"), {
            totalStarted: 1,
            totalStartedExternal: 1,
            totalAborted: 1,
            totalAbortedExternal: 1,
        });
    });

    it("counts a server-initiated transaction as internal", function () {
        const prev = pollAllNodes("before internal txn");

        assert.commandWorked(
            primary.adminCommand({
                testInternalTransactions: 1,
                commandInfos: [
                    {
                        dbName: dbName,
                        command: {insert: collName, documents: [{_id: 3, x: 1}]},
                    },
                ],
            }),
        );

        assertDeltas(prev, pollAllNodes("after internal txn"), {
            totalStarted: 1,
            totalStartedInternal: 1,
            totalCommitted: 1,
            totalCommittedInternal: 1,
        });
    });
});
