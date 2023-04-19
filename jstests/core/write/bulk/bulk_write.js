/**
 * Tests bulk write command for valid input.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # Command is not yet compatible with tenant migration.
 *   tenant_migration_incompatible,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 * ]
 */
(function() {
"use strict";

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

// Make sure a properly formed request has successful result
assert.commandWorked(db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: "test.coll"}]}));

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
coll.drop();

// Make sure optional fields are accepted
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    cursor: {batchSize: 1024},
    bypassDocumentValidation: true,
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
coll.drop();

// Make sure ops and nsInfo can take arrays properly
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 1, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

// Test 2 inserts into the same namespace
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

assert.eq(coll.find().itcount(), 2);
assert.eq(coll1.find().itcount(), 0);
coll.drop();

// Test BypassDocumentValidator
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(db.runCommand({collMod: "coll", validator: {a: {$exists: true}}}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 3, skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    bypassDocumentValidation: true,
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

assert.eq(1, coll.count({_id: 3}));

coll.drop();
})();
