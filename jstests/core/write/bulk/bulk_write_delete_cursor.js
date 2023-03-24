/**
 * Tests bulk write cursor response for correct responses.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   # TODO SERVER-72988: Until bulkWrite is compatible with retryable writes.
 *   requires_non_retryable_writes,
 *   # Command is not yet compatible with tenant migration.
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

// Skip this test if the BulkWriteCommand feature flag is not enabled.
// TODO SERVER-67711: Remove feature flag check.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "BulkWriteCommand")) {
    jsTestLog('Skipping test because the BulkWriteCommand feature flag is disabled.');
    return;
}

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

const cursorEntryValidator = function(entry, expectedEntry) {
    assert(entry.ok == expectedEntry.ok);
    assert(entry.idx == expectedEntry.idx);
    assert(entry.n == expectedEntry.n);
    assert(entry.code == expectedEntry.code);
};

// Test generic delete with no return.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
assert(!res.cursor.firstBatch[1].value);
assert(!res.cursor.firstBatch[2]);

assert(!coll.findOne());

coll.drop();

// Test return.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB"});
assert(!res.cursor.firstBatch[2]);

assert(!coll.findOne());

coll.drop();

// Test only deletes one when multi is false (default value) with sort.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}, sort: {_id: -1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
assert.docEq(res.cursor.firstBatch[2].value, {_id: 1, skey: "MongoDB"});
assert(!res.cursor.firstBatch[3]);
assert.sameMembers(coll.find().toArray(), [{_id: 0, skey: "MongoDB"}]);

coll.drop();

// Test only deletes one when multi is false (default value) with sort.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}, sort: {_id: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
assert.docEq(res.cursor.firstBatch[2].value, {_id: 0, skey: "MongoDB"});
assert(!res.cursor.firstBatch[3]);
assert.sameMembers(coll.find().toArray(), [{_id: 1, skey: "MongoDB"}]);

coll.drop();

// Test deletes multiple when multi is true.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {delete: 0, filter: {skey: "MongoDB"}, multi: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 2});
assert(!res.cursor.firstBatch[3]);
assert(!coll.findOne());

coll.drop();

// Test Insert outside of bulkWrite + delete in bulkWrite.
coll.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
assert.docEq(res.cursor.firstBatch[0].value, {_id: 1, skey: "MongoDB"});
assert(!res.cursor.firstBatch[1]);

assert(!coll.findOne());

coll.drop();

// Test delete matches namespace correctly.
coll1.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 0});
assert(!res.cursor.firstBatch[0].value);
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB", coll1.findOne().skey);

coll.drop();
coll1.drop();

// Make sure multi:true + return fails the op.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}, multi: true, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[1]);

// Test returnFields with return.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 0}, returnFields: {_id: 0, skey: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
assert.docEq(res.cursor.firstBatch[1].value, {skey: "MongoDB"});
assert(!res.cursor.firstBatch[2]);

assert(!coll.findOne());

coll.drop();

// Test providing returnFields without return option.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 0}, returnFields: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[2]);

coll.drop();

// Test let matches specific document.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB3"}},
        {
            delete: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey"]}},
            let : {targetKey: "MongoDB"},
            return: true
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1});
assert.docEq(res.cursor.firstBatch[3].value, {_id: 0, skey: "MongoDB"});
assert(!res.cursor.firstBatch[4]);

assert.sameMembers(coll.find().toArray(), [{_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

coll.drop();

// Test write fails userAllowedWriteNS.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            delete: 0,
            filter: {_id: 1},
            multi: true,
        },
    ],
    nsInfo: [{ns: "test.system.profile"}]
});

assert.commandWorked(res);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidNamespace});
assert(!res.cursor.firstBatch[1]);

// Test delete continues on error with ordered:false.
coll.insert({_id: 1, skey: "MongoDB"});
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            delete: 0,
            filter: {_id: 0},
        },
        {delete: 1, filter: {_id: 1}, return: true}
    ],
    nsInfo: [{ns: "test.system.profile"}, {ns: "test.coll"}],
    ordered: false
});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidNamespace});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB"});
assert(!res.cursor.firstBatch[2]);

assert(!coll.findOne());

coll.drop();

// Test delete stop on error with ordered:true.
coll.insert({_id: 1, skey: "MongoDB"});
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            delete: 0,
            filter: {_id: 0},
        },
        {delete: 1, filter: {_id: 1}, return: true},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
    ],
    nsInfo: [{ns: "test.system.profile"}, {ns: "test.coll"}],
});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidNamespace});
assert(!res.cursor.firstBatch[1]);

assert.eq(coll.findOne().skey, "MongoDB");

coll.drop();
})();
