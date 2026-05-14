/**
 * Verifies fast-count parity between primary and secondary across the DDL oplog entry
 * types that currently skip the fast-count eligibility check on the apply side
 * (kCreate, kDrop, kTruncateRange), plus index DDL (kCreateIndexes, kDropIndexes) and
 * a bulk CRUD mix (insert/update/delete) that exercises the well-covered path as a
 * control.
 *
 * For each row of the matrix we drive the primary, await replication, and then assert
 * three things against every collection that survives the row:
 *   1. fast count via collStats agrees primary == secondary,
 *   2. fast count agrees with the authoritative scan via find().itcount(),
 *   3. dbHash agrees across all data-bearing nodes for the affected db.
 *
 * See SERVER-125890. Coverage focuses on the eligibility-check gap; once the fix lands
 * the matrix should continue to pass without modification.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_83,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const dbName = "oplog_ddl_fast_count_parity";
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

function fastCount(db, collName) {
    const stats = db.runCommand({collStats: collName});
    if (!stats.ok) {
        return null;
    }
    return stats.count;
}

function scanCount(db, collName) {
    return db.getCollection(collName).find().itcount();
}

function assertParity(label, collName, {expectedCount, clustered = false} = {}) {
    rst.awaitReplication();

    const primaryFast = fastCount(primaryDB, collName);
    const secondaryFast = fastCount(secondaryDB, collName);
    const primaryScan = scanCount(primaryDB, collName);
    const secondaryScan = scanCount(secondaryDB, collName);

    jsTest.log.info(`[${label}] ${collName}: primaryFast=${primaryFast}` +
                    ` secondaryFast=${secondaryFast}` +
                    ` primaryScan=${primaryScan} secondaryScan=${secondaryScan}`);

    assert.eq(primaryFast, secondaryFast,
              `[${label}] fast-count drift on ${collName}: primary=${primaryFast}` +
              ` secondary=${secondaryFast}`);
    assert.eq(primaryFast, primaryScan,
              `[${label}] fast-count vs scan drift on primary for ${collName}`);
    assert.eq(secondaryFast, secondaryScan,
              `[${label}] fast-count vs scan drift on secondary for ${collName}`);
    if (typeof expectedCount === "number") {
        assert.eq(primaryFast, expectedCount,
                  `[${label}] unexpected count on ${collName}: got=${primaryFast}` +
                  ` want=${expectedCount}`);
    }

    // dbHash across all data-bearing nodes pins anything collStats wouldn't notice.
    const hashes = rst.getHashes(dbName);
    const primaryHash = hashes.primary.collections[collName];
    for (const secHash of hashes.secondaries) {
        assert.eq(primaryHash, secHash.collections[collName],
                  `[${label}] dbHash drift on ${collName}`);
    }
}

function seed(collName, numDocs, {clustered = false} = {}) {
    const opts = clustered ? {clusteredIndex: {key: {_id: 1}, unique: true}} : {};
    assert.commandWorked(primaryDB.createCollection(collName, opts));
    if (numDocs > 0) {
        const docs = [];
        for (let i = 1; i <= numDocs; i++) {
            docs.push({_id: i, payload: `v${i}`});
        }
        assert.commandWorked(primaryDB.getCollection(collName).insertMany(docs));
    }
}

// -----------------------------------------------------------------------------
// Row 1: kCreate — empty + non-empty, plain + clustered.
// -----------------------------------------------------------------------------

(function rowCreate() {
    const label = "kCreate";
    const emptyColl = "create_empty";
    const seededColl = "create_seeded";
    const clusteredColl = "create_clustered";

    assert.commandWorked(primaryDB.createCollection(emptyColl));
    assertParity(label, emptyColl, {expectedCount: 0});

    seed(seededColl, 25);
    assertParity(label, seededColl, {expectedCount: 25});

    assert.commandWorked(primaryDB.createCollection(
        clusteredColl, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    assert.commandWorked(primaryDB.getCollection(clusteredColl).insertMany(
        [{_id: 1}, {_id: 2}, {_id: 3}]));
    assertParity(label, clusteredColl, {expectedCount: 3, clustered: true});
})();

// -----------------------------------------------------------------------------
// Row 2: kCreateIndexes / kDropIndexes — index DDL should not perturb fast count.
// -----------------------------------------------------------------------------

(function rowIndexDDL() {
    const label = "kCreateIndexes/kDropIndexes";
    const collName = "index_ddl";

    seed(collName, 40);
    assertParity(label + ":pre", collName, {expectedCount: 40});

    assert.commandWorked(primaryDB.getCollection(collName).createIndex({payload: 1}));
    assert.commandWorked(primaryDB.getCollection(collName).createIndex({payload: 1, _id: -1}));
    assertParity(label + ":post-create", collName, {expectedCount: 40});

    assert.commandWorked(primaryDB.getCollection(collName).dropIndex("payload_1"));
    assertParity(label + ":post-drop", collName, {expectedCount: 40});
})();

// -----------------------------------------------------------------------------
// Row 3: bulk CRUD mix — covered eligibility path; control for the matrix.
// -----------------------------------------------------------------------------

(function rowCrudMix() {
    const label = "crud_mix";
    const collName = "crud_mix";

    seed(collName, 0);
    const coll = primaryDB.getCollection(collName);
    const initial = [];
    for (let i = 1; i <= 100; i++) {
        initial.push({_id: i, k: i % 7, payload: `v${i}`});
    }
    assert.commandWorked(coll.insertMany(initial));
    assertParity(label + ":insert", collName, {expectedCount: 100});

    assert.commandWorked(coll.updateMany({k: 0}, {$set: {touched: true}}));
    assertParity(label + ":update", collName, {expectedCount: 100});

    const delRes = assert.commandWorked(coll.deleteMany({k: {$in: [1, 2, 3]}}));
    assertParity(label + ":delete", collName, {expectedCount: 100 - delRes.deletedCount});
})();

// -----------------------------------------------------------------------------
// Row 4: kTruncateRange — drive a replicated truncateRange via applyOps and
// assert parity after each slice. The eligibility-check gap is most user-visible
// here because secondaries decrement the fast count from an apply-side path.
// -----------------------------------------------------------------------------

(function rowTruncateRange() {
    const label = "kTruncateRange";
    const collName = "truncate_range";

    assert.commandWorked(primaryDB.createCollection(
        collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    const coll = primaryDB.getCollection(collName);
    const docs = [];
    for (let i = 1; i <= 60; i++) {
        docs.push({_id: i});
    }
    assert.commandWorked(coll.insertMany(docs));
    assertParity(label + ":seed", collName, {expectedCount: 60, clustered: true});

    function truncateSlice(minId, maxId, expectedCountAfter) {
        const sliceMeta = coll.find({_id: {$gte: minId, $lte: maxId}})
                              .sort({_id: 1})
                              .showRecordId()
                              .toArray();
        assert.gt(sliceMeta.length, 0, `slice [${minId},${maxId}] empty`);
        const minRecordId = sliceMeta[0].$recordId;
        const maxRecordId = sliceMeta[sliceMeta.length - 1].$recordId;
        const recordsDeleted = sliceMeta.length;

        const applyOpsCmd = {
            applyOps: [{
                op: "c",
                ns: `${dbName}.$cmd`,
                o: {
                    truncateRange: coll.getFullName(),
                    minRecordId: minRecordId,
                    maxRecordId: maxRecordId,
                    bytesDeleted: recordsDeleted, // placeholder, see replicated_truncate.js
                    docsDeleted: recordsDeleted,
                },
            }],
        };
        assert.commandWorked(primaryDB.runCommand(applyOpsCmd));
        assertParity(label + `:[${minId},${maxId}]`, collName,
                     {expectedCount: expectedCountAfter, clustered: true});
    }

    // Front, middle, end, then mop up everything that remains.
    truncateSlice(1, 5, 55);
    truncateSlice(30, 35, 49);
    truncateSlice(56, 60, 44);
    truncateSlice(6, 60, 0);
})();

// -----------------------------------------------------------------------------
// Row 5: kDrop — collection drop must zero the fast count on both nodes and
// dbHash must agree (i.e. the dropped collection disappears from both sides).
// -----------------------------------------------------------------------------

(function rowDrop() {
    const label = "kDrop";
    const collName = "drop_seeded";

    seed(collName, 12);
    assertParity(label + ":seed", collName, {expectedCount: 12});

    assert.commandWorked(primaryDB.getCollection(collName).drop());
    rst.awaitReplication();

    assert.eq(fastCount(primaryDB, collName), null,
              `[${label}] primary still reports collStats on dropped ${collName}`);
    assert.eq(fastCount(secondaryDB, collName), null,
              `[${label}] secondary still reports collStats on dropped ${collName}`);

    const hashes = rst.getHashes(dbName);
    assert(!Object.prototype.hasOwnProperty.call(hashes.primary.collections, collName),
           `[${label}] dropped collection still in primary dbHash`);
    for (const secHash of hashes.secondaries) {
        assert(!Object.prototype.hasOwnProperty.call(secHash.collections, collName),
               `[${label}] dropped collection still in secondary dbHash`);
    }
})();

rst.stopSet();
