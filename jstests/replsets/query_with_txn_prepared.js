/**
 * This test attempts to reproduce the bug described in SERVER-48002. This is a best-effort test
 * that will not detect this bug on every run, even if a bug exists.
 *
 * Snapshot isolation cannot be guaranteed for operations that ignore prepare conflicts.
 * This means that two reads of the same record in the same snapshot can return different results.
 * In practice, the DataCorruptionDetected assertion added by SERVER-40620 will trigger if an index
 * points to a non-existent record.
 *
 * Queries that ignore prepare conflicts and use an index to satisfy a read can read a key from an
 * index and fetch a record that appears to go missing within the same snapshot. This may happen
 * when the collection read races with a prepared transaction that commits and deletes the record.
 *
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const dbName = "query_with_txn_prepared";
const collName = "coll";

assert.commandWorked(primary.getDB(dbName)[collName].createIndexes([{x: 1}]));

const transactionShell = startParallelShell(async function () {
    const {PrepareHelpers} = await import("jstests/core/txns/libs/prepare_helpers.js");
    while (db.getSiblingDB("query_with_txn_prepared")["stopQueries"].find().count() == 0) {
        for (let i = 0; i < 100; ++i) {
            const session = db.getMongo().startSession();
            const sessionColl = session.getDatabase("query_with_txn_prepared")["coll"];

            session.startTransaction({readConcern: {level: "majority"}});
            if (Math.random() < 0.5) {
                assert.commandWorked(sessionColl.update({x: 1}, {x: 1}, {upsert: true}));
            } else {
                assert.commandWorked(sessionColl.remove({x: 1}));
            }

            const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
            assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
        }
    }
}, primary.port);

for (let i = 0; i < 2000; ++i) {
    const result = primary.getDB(dbName)[collName].find({x: 1}).toArray();
    // Although there are either 0 or 1 documents in the collection at all times, it's possible
    // for more than 1 document to show up in the query due to yielding mechanics. See
    // SERVER-101381 for more information.
    assert.gte(result.length, 0, result);
}

assert.commandWorked(primary.getDB(dbName)["stopQueries"].insert({stop: 1}));
transactionShell();

replTest.stopSet();
