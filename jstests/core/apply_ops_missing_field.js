/**
 * Test that applyOps ignores updates intended for array elements or subdocument fields, if the
 * target document lacks the expected array/subdocument. This can arise when re-applying past
 * operations that happened before the array/subdoc field was removed. The applyOps command must be
 * able to continue past these errors for the sake of idempotency.
 *
 * Additionally, if an update to one field fails, updates to other fields in the same applyOps
 * operation should succeed.
 *
 * The original motivation for this test is MongoMirror.
 *
 * @tags: [
 *     requires_non_retryable_commands,
 *     # applyOps is not supported on mongos
 *     assumes_against_mongod_not_mongos,
 *     # applyOps uses the oplog which requires replication support.
 *     requires_replication,
 *     # Tenant migrations don't support applyOps.
 *     tenant_migration_incompatible,
 * ]
 */

(function() {
"use strict";

const coll = db.getCollection(jsTestName());
coll.drop();

// Run applyOps as MongoMirror does, with a dummy command to make it non-atomic. MongoMirror does
// not use allowAtomic: false.
function applyOps(ops) {
    ops.push({op: "c", ns: "admin.$cmd", o: {applyOps: [{op: "n", ns: "", o: {"msg": "noop"}}]}});
    let command = {applyOps: ops, writeConcern: {w: "majority"}};
    assert.commandWorked(db.adminCommand(command));
}

const originalDoc = {
    _id: 1,
    a: null
};

coll.insert(originalDoc);

// Update field "a", which is null, but apply an operation intended for an array.
applyOps(
    [{ns: coll.getFullName(), op: "u", o2: {_id: 1}, o: {$v: 2, diff: {sa: {a: true, u0: 1}}}}]);
assert.eq(originalDoc, coll.findOne());

// Same, but also set a new field 'b' to 1.
applyOps([{
    ns: coll.getFullName(),
    op: "u",
    o2: {_id: 1},
    o: {$v: 2, diff: {u: {b: 1}, sa: {a: true, u0: 1}}}
}]);
assert.eq({_id: 1, a: null, b: 1}, coll.findOne());

// An operation intended for a subdocument.
applyOps(
    [{ns: coll.getFullName(), op: "u", o2: {_id: 1}, o: {$v: 2, diff: {sa: {u: {field: 2}}}}}]);
assert.eq({_id: 1, a: null, b: 1}, coll.findOne());

// Same, but set 'b' to 2.
applyOps([{
    ns: coll.getFullName(),
    op: "u",
    o2: {_id: 1},
    o: {$v: 2, diff: {u: {b: 2}, sa: {u: {field: 2}}}}
}]);
assert.eq({_id: 1, a: null, b: 2}, coll.findOne());
}());
