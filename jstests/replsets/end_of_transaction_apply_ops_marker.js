/**
 * Verifies that the final applyOps oplog entry of an unprepared multi-document transaction is
 * marked with 'allowEndOfTransaction: true' only when the end-of-transaction write path is enabled,
 * i.e. both the 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag (enabled via the tag
 * below) and the 'allowEndOfTransaction' server parameter are on. This covers both transactions
 * that fit in a single applyOps entry and transactions that span multiple applyOps entries.
 *
 * @tags: [
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_replication,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {withSession} from "jstests/libs/query/change_stream_util.js";

describe("allowEndOfTransaction marker on the final applyOps entry", function () {
    const dbName = jsTestName();
    const collName = jsTestName();
    const nss = dbName + "." + collName;

    let rst;
    let primary;

    before(function () {
        // Force one operation per applyOps entry so that a transaction with more than one
        // operation spans multiple applyOps entries.
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {maxNumberOfTransactionOperationsInSingleOplogEntry: 1}},
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
    });

    after(function () {
        rst.stopSet();
    });

    function getLatestOplogTs() {
        return primary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next().ts;
    }

    // Runs a multi-document transaction with 'numOps' insert operations and returns the applyOps
    // oplog entries it produced, in timestamp order.
    function runTransactionAndGetApplyOps(numOps) {
        const db = primary.getDB(dbName);

        // Recreate the collection outside of the transaction so that the transaction contains
        // exactly 'numOps' operations and does not add an implicit collection creation.
        assertDropAndRecreateCollection(db, collName);
        const beforeTs = getLatestOplogTs();

        withSession(db.getMongo(), (session) => {
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

    it("marks the single applyOps entry of a single-entry transaction", function () {
        runWithParamsAllNonConfigNodes(primary.getDB(dbName), {allowEndOfTransaction: true}, () => {
            const applyOps = runTransactionAndGetApplyOps(1);
            assert.eq(applyOps.length, 1, "expected a single applyOps entry", {applyOps});
            assert.eq(applyOps[0].o.allowEndOfTransaction, true, "missing marker", {applyOps});
        });
    });

    it("marks only the final applyOps entry of a multi-entry transaction", function () {
        runWithParamsAllNonConfigNodes(primary.getDB(dbName), {allowEndOfTransaction: true}, () => {
            const applyOps = runTransactionAndGetApplyOps(3);
            assert.eq(applyOps.length, 3, "expected three applyOps entries", {applyOps});

            for (let i = 0; i < applyOps.length - 1; i++) {
                assert(
                    !applyOps[i].o.hasOwnProperty("allowEndOfTransaction"),
                    "unexpected marker on a non-final applyOps entry",
                    {entry: applyOps[i]},
                );
                assert.eq(applyOps[i].o.partialTxn, true, "expected partialTxn", {
                    entry: applyOps[i],
                });
            }

            const last = applyOps[applyOps.length - 1];
            assert.eq(last.o.allowEndOfTransaction, true, "missing marker on final entry", {last});
            assert.eq(last.o.count, 3, "unexpected count on final entry", {last});
        });
    });

    it("does not mark any applyOps entry when the parameter is disabled", function () {
        runWithParamsAllNonConfigNodes(
            primary.getDB(dbName),
            {allowEndOfTransaction: false},
            () => {
                const applyOps = runTransactionAndGetApplyOps(2);
                assert.gt(applyOps.length, 0, "expected applyOps entries", {applyOps});
                for (const entry of applyOps) {
                    assert(
                        !entry.o.hasOwnProperty("allowEndOfTransaction"),
                        "unexpected marker while the parameter is disabled",
                        {entry},
                    );
                }
            },
        );
    });
});
