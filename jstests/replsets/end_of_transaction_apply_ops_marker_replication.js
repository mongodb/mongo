/**
 * Verifies that the 'allowEndOfTransaction' marker written on the final applyOps oplog entry of an
 * unprepared multi-document transaction is replicated verbatim from the replica-set primary to the
 * secondaries.
 *
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter
 * is enabled per case. One operation per applyOps entry is forced so that a transaction with more
 * than one operation spans multiple applyOps entries, and only the final entry is marked.
 *
 * @tags: [
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_fcv_90,
 *   requires_replication,
 *   uses_transactions,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

describe("allowEndOfTransaction marker replicated to secondaries", function () {
    const dbName = jsTestName();
    const collName = jsTestName();
    const nss = dbName + "." + collName;

    let rst;
    let primary;
    let secondary;

    before(function () {
        // Force one operation per applyOps entry so that a transaction with more than one
        // operation spans multiple applyOps entries. Two nodes so that the entry replicates.
        rst = new ReplSetTest({
            nodes: 2,
            nodeOptions: {setParameter: {maxNumberOfTransactionOperationsInSingleOplogEntry: 1}},
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();
    });

    after(function () {
        rst.stopSet();
    });

    function getLatestOplogTs() {
        return primary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next().ts;
    }

    // Runs a multi-document transaction with 'numOps' insert operations and returns the applyOps
    // oplog entries it produced on the primary, in timestamp order.
    function runTransactionAndGetApplyOps(numOps) {
        const db = primary.getDB(dbName);

        // Recreate the collection outside of the transaction so that the transaction contains
        // exactly 'numOps' operations and does not add an implicit collection creation.
        assertDropAndRecreateCollection(db, collName);
        const beforeTs = getLatestOplogTs();

        withSession(primary, (session) => {
            const sessionColl = session.getDatabase(dbName).getCollection(collName);
            session.startTransaction();
            for (let i = 0; i < numOps; i++) {
                assert.commandWorked(sessionColl.insertOne({_id: i}, {session}));
            }
            assert.commandWorked(session.commitTransaction_forTesting());
        });

        return primary
            .getDB("local")
            .oplog.rs.find({ts: {$gt: beforeTs}, "o.applyOps": {$elemMatch: {ns: nss}}})
            .sort({ts: 1})
            .toArray();
    }

    // Asserts that only the final entry in 'entries' carries the marker and that no earlier entry
    // does. 'label' identifies the node in failure messages.
    function assertOnlyLastEntryMarked(entries, label) {
        for (let i = 0; i < entries.length - 1; i++) {
            assert(
                !entries[i].o.hasOwnProperty("allowEndOfTransaction"),
                "a non-final applyOps entry carries the marker on the " + label,
                {entry: entries[i]},
            );
        }
        const last = entries[entries.length - 1];
        assert.eq(last.o.allowEndOfTransaction, true, "missing marker on the " + label, {last});
    }

    it("replicates the marker on the final applyOps entry to the secondary", function () {
        runWithParamsAllNonConfigNodes(primary.getDB(dbName), {allowEndOfTransaction: true}, () => {
            const applyOps = runTransactionAndGetApplyOps(3);
            assert.eq(applyOps.length, 3, "expected three applyOps entries", {applyOps});
            assertOnlyLastEntryMarked(applyOps, "primary");

            // Wait for the entries to reach the secondary and fetch them by ts.
            rst.awaitReplication();
            const secondaryEntries = applyOps.map((entry) => {
                const secondaryEntry = secondary.getDB("local").oplog.rs.findOne({ts: entry.ts});
                assert(secondaryEntry, "an applyOps entry did not replicate to the secondary", {
                    ts: entry.ts,
                });
                return secondaryEntry;
            });

            // The marker must be replicated verbatim, and only on the final entry.
            assertOnlyLastEntryMarked(secondaryEntries, "secondary");
        });
    });
});
