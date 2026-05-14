/**
 * Regression test: validate (and, when accessible, dbCheck) must not report false-positive
 * "extra index entry" / index inconsistency warnings for multikey indexes whose data contains
 * BSON-equivalent numeric values across distinct BSON types (e.g. NumberInt(1), NumberLong(1),
 * NumberDouble(1.0)).
 *
 * Background:
 *   On a multikey index, an array element of each numeric BSON type is generated as its own
 *   index entry, but WiredTiger's KeyString collapses BSON-equivalent numeric values to a
 *   single storage key (with distinct type-bits suffixes). Earlier validate logic compared
 *   the de-duplicated storage-key set against the document-derived multikey set without
 *   normalizing numeric types, producing a count mismatch and an "extra index entries" warning
 *   on an otherwise-correct index. The fix normalizes numeric type before set comparison in
 *   the index-consistency phase so equivalent values collapse on both sides.
 *
 * The test asserts:
 *   1. {full: true} validate returns valid:true with empty errors and zero extraIndexEntries
 *      across all index details.
 *   2. The multikey index reports the expected per-document key count without flagging the
 *      collection as invalid.
 *   3. dbCheck (when available on the build) produces no batch-level inconsistency entries
 *      in db.local.system.healthlog for the test namespace.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "validate_multikey_equivalent_values";
const testDB = primary.getDB(dbName);
const coll = testDB.getCollection(collName);

assert.commandWorked(testDB.createCollection(collName));
assert.commandWorked(coll.createIndex({arr: 1}));

// Insert documents whose arrays contain BSON-equivalent numeric values across distinct numeric
// types. After KeyString encoding these collapse to one storage key per logical value but the
// catalog still generates one index entry per array element.
assert.commandWorked(coll.insert({_id: 0, arr: [1, NumberLong(1), 1.0]}));
assert.commandWorked(coll.insert({_id: 1, arr: [NumberInt(2), NumberLong(2), 2.0, NumberDecimal("2.0")]}));
assert.commandWorked(coll.insert({_id: 2, arr: [NumberLong(3)]}));

// Sanity-check the documents landed.
assert.eq(3, coll.find().itcount(), "unexpected document count after insert");

jsTestLog("Running coll.validate({full: true})");
const result = assert.commandWorked(coll.validate({full: true}));
jsTestLog("Validation result: " + tojson(result));

// Top-level invariants: valid, no errors, no warnings about extra entries.
assert.eq(coll.getFullName(), result.ns, tojson(result));
assert(result.valid, "validate reported invalid for equivalent-numeric multikey data: " + tojson(result));
assert.eq([], result.errors, "validate produced top-level errors: " + tojson(result));
assert.eq(3, result.nrecords, tojson(result));

// Per-index invariants on the multikey index: must be marked multikey, valid, and free of
// extra/missing index entries. Validate must NOT collapse the per-element index entries before
// comparison; it must normalize numeric types on both sides.
const indexName = "arr_1";
const indexDetails = result.indexDetails[indexName];
assert(indexDetails, "missing indexDetails for " + indexName + ": " + tojson(result));
assert(indexDetails.valid, "multikey index reported invalid: " + tojson(indexDetails));
assert.eq([], indexDetails.errors || [], "index " + indexName + " produced errors: " + tojson(indexDetails));
assert.eq([], indexDetails.warnings || [], "index " + indexName + " produced warnings: " + tojson(indexDetails));

// _id index also expected clean.
const idDetails = result.indexDetails._id_;
assert(idDetails.valid, "_id index reported invalid: " + tojson(idDetails));

// keysPerIndex on arr_1 should equal the total number of array elements indexed (3 + 4 + 1 = 8).
// The point of this assertion is that none of the entries got pruned or double-counted under
// the equivalent-numeric collapse.
assert.eq(8, result.keysPerIndex[indexName],
          "multikey index key-count mismatch (false positive surface): " + tojson(result));

// Defensive scan for any extraIndexEntries / missingIndexEntries shapes in the result, in case
// future versions surface them at the top level rather than per-index.
if (result.hasOwnProperty("extraIndexEntries")) {
    assert.eq(0, result.extraIndexEntries.length,
              "validate flagged extra index entries on equivalent-numeric multikey data: " +
              tojson(result.extraIndexEntries));
}
if (result.hasOwnProperty("missingIndexEntries")) {
    assert.eq(0, result.missingIndexEntries.length,
              "validate flagged missing index entries on equivalent-numeric multikey data: " +
              tojson(result.missingIndexEntries));
}

// Optional dbCheck pass. dbCheck is an admin command and may not be enabled on every build
// configuration; treat command failure as skip rather than test failure. When it does run,
// assert no batch-level inconsistency rows land in the health log for this namespace.
jsTestLog("Attempting dbCheck on " + coll.getFullName());
const dbCheckRes = testDB.runCommand({dbCheck: collName});
if (dbCheckRes.ok) {
    // Wait for dbCheck to drain through the health log.
    assert.soon(
        () => {
            const healthLog = primary.getDB("local").system.healthlog;
            const finishLog = healthLog.findOne({operation: "dbCheckStop", "data.success": true});
            return finishLog !== null;
        },
        "dbCheck did not finish in time",
        60 * 1000,
    );
    const healthLog = primary.getDB("local").system.healthlog;
    const inconsistencies = healthLog
        .find({
            namespace: coll.getFullName(),
            severity: {$in: ["error", "warning"]},
        })
        .toArray();
    assert.eq([], inconsistencies,
              "dbCheck reported inconsistencies on equivalent-numeric multikey data: " +
              tojson(inconsistencies));
} else {
    jsTestLog("dbCheck unavailable on this build (" + tojson(dbCheckRes) +
              "); skipping dbCheck assertions.");
}

rst.stopSet();
