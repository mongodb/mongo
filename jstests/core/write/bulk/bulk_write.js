/**
 * Tests bulk write command for valid / invalid input.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   requires_fastcount,
 *   # Until bulkWrite is compatible with retryable writes.
 *   requires_non_retryable_writes,
 *   # Command is not yet compatible with tenant migration.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

// Skip this test if the BulkWriteCommand feature flag is not enabled
if (!FeatureFlagUtil.isEnabled(db, "BulkWriteCommand")) {
    jsTestLog('Skipping test because the BulkWriteCommand feature flag is disabled.');
    return;
}

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

// Make sure a properly formed request has successful result
assert.commandWorked(db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]}));

assert.eq(coll.count(), 1);
assert.eq(coll1.count(), 0);
coll.drop();

// Make sure non-adminDB request fails
assert.commandFailedWithCode(db.runCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}),
                             [ErrorCodes.Unauthorized]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Make sure optional fields are accepted
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    cursor: {batchSize: 1024},
    bypassDocumentValidation: true,
    ordered: false
}));

assert.eq(coll.count(), 1);
assert.eq(coll1.count(), 0);
coll.drop();

// Make sure invalid fields are not accepted
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    cursor: {batchSize: 1024},
    bypassDocumentValidation: true,
    ordered: false,
    fooField: 0
}),
                             [40415]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Make sure ops and nsInfo can take arrays properly
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
}));

assert.eq(coll.count(), 1);
assert.eq(coll1.count(), 1);
coll.drop();
coll1.drop();

// Test 2 inserts into the same namespace
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}));

assert.eq(coll.count(), 2);
assert.eq(coll1.count(), 0);
coll.drop();

// Make sure we fail if index out of range of nsInfo
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 2, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
}),
                             [ErrorCodes.BadValue]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Missing ops
assert.commandFailedWithCode(db.adminCommand({bulkWrite: 1, nsInfo: [{ns: "mydb.coll"}]}), [40414]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Missing nsInfo
assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}]}), [40414]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Test valid arguments with invalid values
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: "test", document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}),
                             [ErrorCodes.TypeMismatch]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: "test"}], nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: ["test"]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: "test", nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: "test"}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.count(), 0);
assert.eq(coll1.count(), 0);

// Test 2 inserts into the same namespace
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}));

assert.eq(coll.count(), 2);
assert.eq(coll1.count(), 0);
coll.drop();

// Test that a write can fail part way through a write and the write partially executes.
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
}));

assert.eq(coll.count(), 1);
assert.eq(coll1.count(), 0);
coll.drop();
coll1.drop();

assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
    ordered: false
}));

assert.eq(coll.count(), 1);
assert.eq(coll1.count(), 1);
coll.drop();
coll1.drop();

// Test BypassDocumentValidator
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(db.runCommand({collMod: "coll", validator: {a: {$exists: true}}}));

assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 3, skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    bypassDocumentValidation: false,
}));

assert.eq(0, coll.count({_id: 3}));

assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 3, skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    bypassDocumentValidation: true,
}));

assert.eq(1, coll.count({_id: 3}));
})();
