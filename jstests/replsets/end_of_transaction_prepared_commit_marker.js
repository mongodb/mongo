/**
 * Verifies that the 'allowEndOfTransaction' marker that a prepared replica-set transaction writes
 * into its 'commitTransaction' oplog entry is written on the entry's 'o2' (not on its 'o') on the
 * primary and replicated verbatim to the secondaries.
 *
 * The marker goes on 'o2' because 'commitTransaction.o' is parsed strictly and an older binary
 * would reject an unknown field there, whereas 'o2' is a generic, already-declared oplog field.
 *
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter
 * is toggled per case.
 *
 * @tags: [
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_fcv_90,
 *   requires_replication,
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

describe("allowEndOfTransaction marker on a prepared transaction's commit entry", function () {
    const dbName = jsTestName();
    const collName = jsTestName();

    let rst;
    let primary;
    let secondaries;

    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondaries = rst.getSecondaries();
        assertDropAndRecreateCollection(primary.getDB(dbName), collName);
    });

    after(function () {
        rst.stopSet();
    });

    // Runs a prepared transaction with two inserts and returns the 'commitTransaction' oplog entry
    // the primary wrote for it.
    function runPreparedTransactionAndGetCommitEntry(id) {
        const beforeTs = primary
            .getDB("local")
            .oplog.rs.find()
            .sort({$natural: -1})
            .limit(1)
            .next().ts;

        withSession(primary, (session) => {
            const sessionColl = session.getDatabase(dbName).getCollection(collName);
            session.startTransaction();
            assert.commandWorked(sessionColl.insertOne({_id: id}, {session}));
            assert.commandWorked(sessionColl.insertOne({_id: id + 1}, {session}));
            const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
            assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
        });

        const entries = primary
            .getDB("local")
            .oplog.rs.find({ts: {$gt: beforeTs}, "o.commitTransaction": 1})
            .sort({ts: 1})
            .toArray();
        assert.eq(1, entries.length, "expected exactly one commitTransaction entry", {entries});
        return entries[0];
    }

    it("writes the marker on 'o2' and replicates it to the secondaries", function () {
        runWithParamsAllNonConfigNodes(primary.getDB(dbName), {allowEndOfTransaction: true}, () => {
            const entry = runPreparedTransactionAndGetCommitEntry(1);
            assert.eq(
                entry.o2 && entry.o2.allowEndOfTransaction,
                true,
                "missing marker on 'o2' on the primary",
                {entry},
            );
            assert(
                !entry.o.hasOwnProperty("allowEndOfTransaction"),
                "the marker must not be on 'o'",
                {entry},
            );

            // Wait for the commit entry to reach the secondaries and fetch the same entry by ts.
            rst.awaitReplication();
            for (const secondary of secondaries) {
                const secondaryEntry = secondary.getDB("local").oplog.rs.findOne({ts: entry.ts});
                assert(secondaryEntry, "the commit entry did not replicate to a secondary", {
                    ts: entry.ts,
                });
                assert.eq(
                    secondaryEntry.o2 && secondaryEntry.o2.allowEndOfTransaction,
                    true,
                    "missing marker on 'o2' on a secondary",
                    {secondaryEntry},
                );
            }
        });
    });

    it("writes no marker when the parameter is disabled", function () {
        runWithParamsAllNonConfigNodes(
            primary.getDB(dbName),
            {allowEndOfTransaction: false},
            () => {
                const entry = runPreparedTransactionAndGetCommitEntry(10);
                assert(
                    !(entry.o2 && entry.o2.allowEndOfTransaction),
                    "unexpected marker while the parameter is disabled",
                    {entry},
                );
            },
        );
    });
});
