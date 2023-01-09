/*
 * The test runs commands that are not allowed with security token: applyOps.
 * @tags: [
 *   not_allowed_with_security_token,
 *   requires_non_retryable_commands,
 *   requires_fastcount,
 *   # 6.2 removes support for atomic applyOps
 *   requires_fcv_62,
 *   # applyOps is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # applyOps uses the oplog that require replication support
 *   requires_replication,
 *   # Tenant migrations don't support applyOps.
 *   tenant_migration_incompatible,
 * ]
 */

(function() {
"use strict";
var t = db.apply_ops_dups;
t.drop();

// Make sure the collection exists and create a unique index.
assert.commandWorked(t.insert({_id: 0, x: 1}));
assert.commandWorked(t.createIndex({x: 1}, {unique: true}));

// TODO(SERVER-46221): These oplog entries are inserted as given.  After SERVER-21700 and with
// steady-state oplog constraint enforcement on, they will result in secondary crashes because an
// insert is not treated as an upsert in secondary application mode.  We will need to actually
// convert the second op to an update on the primary, or reject the applyOps on the primary.
if (false) {
    // Check that duplicate _id fields don't cause an error
    var a = assert.commandWorked(db.adminCommand({
        applyOps: [
            {"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: -1}},
            {"op": "i", "ns": t.getFullName(), "o": {_id: 5, x: 0}}
        ]
    }));
    printjson(a);
    printjson(t.find().toArray());
    assert.eq(2, t.find().count(), "Invalid insert worked");
    assert.eq(true, a.results[0], "Valid insert was rejected");
    assert.eq(true, a.results[1], "Insert should have not failed (but should be ignored");
    printjson(t.find().toArray());
}

const prevCount = t.find().count();

// Check that dups on non-id cause errors
var a = assert.commandFailedWithCode(db.adminCommand({
    applyOps: [
        {"op": "i", "ns": t.getFullName(), "o": {_id: 1, x: 0}},
        {"op": "i", "ns": t.getFullName(), "o": {_id: 2, x: 1}}
    ]
}),
                                     ErrorCodes.DuplicateKey);
// We do not applyOps atomically, so the first op is applied and the second is not. The total number
// is now 2.
assert.eq(2, t.find().count(), "Expected 2 documents after applyOps");
})();
