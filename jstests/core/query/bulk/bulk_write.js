/**
 * Tests bulk write command for valid input.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_80
 * ]
 */

const coll = db[jsTestName()];
const collName = coll.getFullName();
const coll1 = db[jsTestName() + "1"];
const coll1Name = coll1.getFullName();

coll.drop();
coll1.drop();

// Make sure a properly formed request has successful result
assert.commandWorked(db.adminCommand(
    {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: [{ns: collName}]}));

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
assert(coll.drop());

// Make sure optional fields are accepted
let res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: collName}],
    cursor: {batchSize: 1024},
    bypassDocumentValidation: true,
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.nErrors, 0, "bulkWrite command response: " + tojson(res));

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
assert(coll.drop());

// Make sure ops and nsInfo can take arrays properly and perform writes to the correct namespace.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 1, document: {skey: "MongoDB"}},
        {insert: 0, document: {skey: "MongoDB"}},
        {insert: 1, document: {_id: 1}}
    ],
    nsInfo: [{ns: collName}, {ns: coll1Name}]
});

assert.commandWorked(res);
assert.eq(res.nErrors, 0, "bulkWrite command response: " + tojson(res));

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 2);
assert(coll.drop());
assert(coll1.drop());

// Test 2 inserts into the same namespace
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
assert.eq(res.nErrors, 0, "bulkWrite command response: " + tojson(res));

assert.eq(coll.find().itcount(), 2);
assert.eq(coll1.find().itcount(), 0);
assert(coll.drop());

// Test BypassDocumentValidator
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(db.runCommand({collMod: coll.getName(), validator: {a: {$exists: true}}}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 3, skey: "MongoDB"}}],
    nsInfo: [{ns: collName}],
    bypassDocumentValidation: true,
});

assert.commandWorked(res);
assert.eq(res.nErrors, 0, "bulkWrite command response: " + tojson(res));

assert.eq(1, coll.count({_id: 3}));

assert(coll.drop());
