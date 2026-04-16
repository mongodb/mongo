/**
 * Verifies that fast size and count is accurate after an unclean shutdown that ocurred before size and count deltas were persisted.
 *
 * @tags: [
 *   featureFlagReplicatedFastCount,
 *   requires_replication,
 *   requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());
const coll = db.getCollection(jsTestName());

const kNumBaselineDocs = 10;
const sampleDocSize = Object.bsonsize({_id: new ObjectId(), x: 1});

function getActualSize(coll) {
    return coll
        .find()
        .map((doc) => Object.bsonsize(doc))
        .reduce((a, b) => a + b, 0);
}

function getFastSize(db, coll) {
    return db.runCommand({dataSize: coll.getFullName()}).size;
}

for (let i = 0; i < kNumBaselineDocs; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Trigger a metadata checkpoint.
{
    const fp = configureFailPoint(db, "sleepAfterFlush");
    assert.commandWorked(db.adminCommand({fsync: 1}));
    fp.wait();
    fp.off();
}

assert.eq(kNumBaselineDocs, coll.find().itcount(), "Actual count should be correct after flush");
assert.eq(kNumBaselineDocs * sampleDocSize, getActualSize(coll), "Actual size should be correct after flush");

assert.eq(coll.count(), coll.find().itcount(), "Fast count should match actual count");
assert.eq(getFastSize(db, coll), getActualSize(coll), "Fast size should match actual size");

const kNumExtraDocs = 5;
const kNumTotalDocs = kNumBaselineDocs + kNumExtraDocs;

const hangFp = configureFailPoint(db, "hangBeforePersistingNewFastCountEntries");

for (let i = 0; i < kNumExtraDocs; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

assert.commandWorked(db.adminCommand({fsync: 1}));

// Wait until the replicated fast count thread is hanging before flushing new size deltas.
hangFp.wait();

rst.stop(primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true, waitpid: true});

rst.start(primary, undefined, /*restart=*/ true);
rst.awaitNodesAgreeOnPrimary();

const primaryAfterRestart = rst.getPrimary();
const dbAfterRestart = primaryAfterRestart.getDB(jsTestName());
const collAfterRestart = dbAfterRestart.getCollection(jsTestName());

assert.eq(kNumTotalDocs, collAfterRestart.find().itcount(), "Actual count should be correct after unclean shutdown");
assert.eq(
    kNumTotalDocs * sampleDocSize,
    getActualSize(collAfterRestart),
    "Actual size should be correct after unclean shutdown",
);
assert.eq(
    collAfterRestart.count(),
    collAfterRestart.find().itcount(),
    "Fast count should match actual count after unclean shutdown",
);
assert.eq(
    getFastSize(dbAfterRestart, collAfterRestart),
    getActualSize(collAfterRestart),
    "Fast size should match actual size after unclean shutdown",
);

rst.stopSet();
