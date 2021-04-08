/**
 * Tests inserting duplicate _id values on a collection clustered by _id.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_wiredtiger,
 * ]
 */

(function() {
"use strict";

// Although this test is tagged with 'requires_wiredtiger', this is not sufficient for ensuring
// that the parallel suite runs this test only on WT configurations.
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger") {
    jsTest.log("Skipping test on non-WT storage engine: " + jsTest.options().storageEngine);
    return;
}

const collName = 'system.buckets.test';
const coll = db[collName];
coll.drop();

assert.commandWorked(db.createCollection(collName, {clusteredIndex: {}}));

// Expect that duplicates are rejected.
let oid = new ObjectId();
assert.commandWorked(coll.insert({_id: oid}));
assert.commandFailedWithCode(coll.insert({_id: oid}), ErrorCodes.DuplicateKey);
assert.eq(1, coll.find({_id: oid}).itcount());

// Updates should work.
assert.commandWorked(coll.update({_id: oid}, {a: 1}));
assert.eq(1, coll.find({_id: oid}).itcount());
})();
