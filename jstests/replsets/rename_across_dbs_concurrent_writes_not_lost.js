/**
 * SERVER-101428: cross-database renameCollection can drop writes acked during the unlocked window
 * between the clone phase (P1) and the rename/drop phase (P2/P3) of `renameCollectionAcrossDatabases`
 * (src/mongo/db/shard_role/shard_catalog/rename_collection.cpp).
 *
 * Repro shape:
 *   - Start a 1-node replica set.
 *   - Seed a source collection with N pre-existing docs in db `srcDB`.
 *   - Kick off a parallel shell that issues a steady stream of inserts targeting `srcDB.coll`
 *     with `w: "majority", j: true` (writes are acked only after journaling -- if the write was
 *     not durable, the parallel shell would surface the error to the assert).
 *   - Concurrently drive a renameCollection moving `srcDB.coll` -> `dstDB.coll`.
 *   - After both finish, count documents in `dstDB.coll` and compare against the count of
 *     acknowledged inserts (preExisting + concurrently-acked). The two MUST be equal: every ack'd
 *     write must be readable from the post-rename target collection.
 *
 * Under the buggy schedule (SERVER-101428), inserts that land in the unlocked window between P1
 * and P3 are acked into the original `source` collection, then `source` is dropped in P3 -- those
 * writes are neither in `dst.coll` (they never went through tmp) nor anywhere else. The
 * post-rename count will be strictly less than the acked count.
 *
 * Under the fix (source MODE_S/MODE_X held for the full P1..P3 duration), the parallel-shell
 * inserts either complete before the rename starts (and therefore appear in `dst.coll` via the
 * clone), or they block until P3 has dropped `source` and observe NamespaceNotFound (which the
 * parallel-shell handles by not counting them as acked). Either way: ack count == final count.
 *
 * @tags: [
 *     requires_replication,
 *     requires_majority_read_concern,
 *     does_not_support_stepdowns,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const replTest = new ReplSetTest({name: "rename_across_dbs_concurrent_writes_not_lost", nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const srcDBName = "src_db_w3_33";
const dstDBName = "dst_db_w3_33";
const collName = "rename_concurrent";
const srcNs = `${srcDBName}.${collName}`;
const dstNs = `${dstDBName}.${collName}`;

const srcDB = primary.getDB(srcDBName);
const dstDB = primary.getDB(dstDBName);

// Seed source with a known number of documents that get cloned to tmp in P1.
const preExistingCount = 500;
{
    const bulk = srcDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < preExistingCount; ++i) {
        bulk.insert({_id: `pre_${i}`, kind: "preExisting"});
    }
    assert.commandWorked(bulk.execute({w: "majority", j: true}));
    assert.eq(preExistingCount, srcDB[collName].countDocuments({}));
}

// Launch a parallel shell that hammers the source namespace with single-doc inserts at majority
// write concern. The shell returns the number of inserts whose getLastError-equivalent succeeded
// (the only ones the client is allowed to assume durably landed).
//
// The shell stops when it sees NamespaceNotFound (the rename has committed and `source` was
// dropped), which is the expected terminal state on the source side.
const writerShell = startParallelShell(
    funWithArgs(function (srcNsArg, ackedCountField) {
        const [srcDBName, collName] = srcNsArg.split(".");
        const writerDB = db.getSiblingDB(srcDBName);
        const accounting = db.getSiblingDB("admin")[ackedCountField];
        // Drop any prior accounting state from a previous run, just in case.
        accounting.drop();
        accounting.insertOne({_id: "acked", n: 0});

        const deadline = Date.now() + 30 * 1000; // hard cap so the test cannot hang.
        let i = 0;
        while (Date.now() < deadline) {
            const doc = {_id: `race_${i}`, kind: "racing"};
            const res = writerDB.runCommand({
                insert: collName,
                documents: [doc],
                writeConcern: {w: "majority", j: true},
            });
            // Per-doc batched insert returns n: number of inserts that succeeded. If the namespace
            // is gone we stop. If we got a write error, do not count it.
            if (!res.ok || res.code === ErrorCodes.NamespaceNotFound) {
                break;
            }
            if (res.writeErrors && res.writeErrors.length > 0) {
                const we = res.writeErrors[0];
                if (we.code === ErrorCodes.NamespaceNotFound) {
                    break;
                }
                // Any other write error: do not count, continue.
            } else if (res.n === 1) {
                accounting.updateOne({_id: "acked"}, {$inc: {n: 1}});
            }
            i++;
        }
    }, srcNs, "ackedAccounting_w3_33"),
    primary.port,
);

// Give the writer a brief head start so the rename hits a non-empty stream.
sleep(250);

// Drive the cross-DB rename.
const renameRes = primary.adminCommand({
    renameCollection: srcNs,
    to: dstNs,
    dropTarget: false,
});
assert.commandWorked(renameRes, () => `renameCollection failed: ${tojson(renameRes)}`);

// Wait for the writer to finish (either it observed NamespaceNotFound or hit the 30s deadline).
writerShell();

// Read the acked count durably journaled by the parallel shell.
const acked = primary.getDB("admin").ackedAccounting_w3_33.findOne({_id: "acked"});
assert(acked, "parallel-shell accounting record missing");
const ackedRacingCount = acked.n;

// Verify the target namespace exists and the source is gone.
assert(dstDB[collName].exists(), `expected ${dstNs} to exist after rename`);
assert(!srcDB[collName].exists(), `expected ${srcNs} to be dropped after rename`);

const finalCount = dstDB[collName].countDocuments({});
const expectedCount = preExistingCount + ackedRacingCount;

jsTestLog(
    `[SERVER-101428] preExisting=${preExistingCount} ackedConcurrent=${ackedRacingCount} ` +
        `finalCount=${finalCount} expected=${expectedCount}`,
);

// The headline assertion. Under the bug this fails when at least one concurrent insert was acked
// in the P1->P3 unlocked window. Under the fix this holds for every interleaving.
assert.eq(
    expectedCount,
    finalCount,
    `Lost write detected (SERVER-101428): expected ${expectedCount} docs in ${dstNs} ` +
        `(${preExistingCount} pre-existing + ${ackedRacingCount} concurrently acked), ` +
        `found ${finalCount}. Missing ${expectedCount - finalCount} write(s).`,
);

replTest.stopSet();
