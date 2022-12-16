/**
 * Tests bulk write command for valid / invalid input.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   not_allowed_with_security_token,
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

// Make sure a properly formed request has successful result
assert.commandWorked(db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]}));

// Make sure non-adminDB request fails
assert.commandFailedWithCode(db.runCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}),
                             [ErrorCodes.Unauthorized]);

// Make sure optional fields are accepted
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    cursor: {batchSize: 1024},
    bypassDocumentValidation: true,
    ordered: false
}));

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

// Make sure ops and nsInfo can take arrays properly
assert.commandWorked(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.mycoll1"}]
}));

// Make sure we fail if index out of range of nsInfo
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 2, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.mycoll1"}]
}),
                             [ErrorCodes.BadValue]);

// Missing ops
assert.commandFailedWithCode(db.adminCommand({bulkWrite: 1, nsInfo: [{ns: "mydb.coll"}]}), [40414]);

// Missing nsInfo
assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}]}), [40414]);

// Test valid arguments with invalid values
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: "test", document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}),
                             [ErrorCodes.TypeMismatch]);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: "test"}], nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: ["test"]}),
    [ErrorCodes.TypeMismatch]);

assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: "test", nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: "test"}),
    [ErrorCodes.TypeMismatch]);
})();
