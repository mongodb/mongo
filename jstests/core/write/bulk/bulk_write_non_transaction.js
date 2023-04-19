/**
 * Tests bulk write command for scenarios that cause the command to fail (ok: 0).
 *
 * These tests are incompatible with the transaction overrides since any failure
 * will cause a transaction abortion which will make the overrides infinite loop.
 *
 * The test runs commands that are not allowed with security token: bulkWrite.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
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

const cursorEntryValidator = function(entry, expectedEntry) {
    assert(entry.ok == expectedEntry.ok);
    assert(entry.idx == expectedEntry.idx);
    assert(entry.n == expectedEntry.n);
    assert(entry.nModified == expectedEntry.nModified);
    assert(entry.code == expectedEntry.code);
};

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

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Make sure we fail if index out of range of nsInfo
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 2, document: {skey: "MongoDB"}}, {insert: 0, document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
}),
                             [ErrorCodes.BadValue]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Missing ops
assert.commandFailedWithCode(db.adminCommand({bulkWrite: 1, nsInfo: [{ns: "mydb.coll"}]}), [40414]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Missing nsInfo
assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}]}), [40414]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Test valid arguments with invalid values
assert.commandFailedWithCode(db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: "test", document: {skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}]
}),
                             [ErrorCodes.TypeMismatch]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: "test"}], nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: ["test"]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: "test", nsInfo: [{ns: "test.coll"}]}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

assert.commandFailedWithCode(
    db.adminCommand(
        {bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}], nsInfo: "test"}),
    [ErrorCodes.TypeMismatch]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Make sure update multi:true + return fails the op.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            multi: true,
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[1]);

// Test update providing returnFields without return option.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {
            update: 0,
            filter: {_id: 0},
            updateMods: {$set: {skey: "MongoDB2"}},
            returnFields: {_id: 1}
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[2]);

coll.drop();

// Test update fails userAllowedWriteNS.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
        },
    ],
    nsInfo: [{ns: "test.system.profile"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidNamespace});
assert(!res.cursor.firstBatch[1]);

var coll2 = db.getCollection("coll2");
coll2.drop();

// Test update continues on error with ordered:false.
assert.commandWorked(coll2.createIndex({x: 1}, {unique: true}));
assert.commandWorked(coll2.insert({x: 3}));
assert.commandWorked(coll2.insert({x: 4}));
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {x: 3}, updateMods: {$inc: {x: 1}}, upsert: true},
        {
            update: 1,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            upsert: true,
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll2"}, {ns: "test.coll"}],
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.DuplicateKey});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 0});
assert.docEq(res.cursor.firstBatch[1].upserted, {index: 0, _id: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[2]);
coll.drop();
coll2.drop();

// Test update stop on error with ordered:true.
assert.commandWorked(coll2.createIndex({x: 1}, {unique: true}));
assert.commandWorked(coll2.insert({x: 3}));
assert.commandWorked(coll2.insert({x: 4}));
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {x: 3}, updateMods: {$inc: {x: 1}}, upsert: true},
        {
            update: 1,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            upsert: true,
            return: "post"
        },
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
    ],
    nsInfo: [{ns: "test.coll2"}, {ns: "test.coll"}],
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.DuplicateKey});
assert(!res.cursor.firstBatch[1]);
coll.drop();
coll2.drop();

// Test fixDocumentForInsert works properly by erroring out on >16MB size insert.
var targetSize = (16 * 1024 * 1024) + 1;
var doc = {_id: new ObjectId(), value: ''};

var size = Object.bsonsize(doc);
assert.gte(targetSize, size);

// Set 'value' as a string with enough characters to make the whole document 'targetSize'
// bytes long.
doc.value = new Array(targetSize - size + 1).join('x');
assert.eq(targetSize, Object.bsonsize(doc));

// Testing ordered:false continues on with other ops when fixDocumentForInsert fails.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: doc},
        {insert: 0, document: {_id: 2, skey: "MongoDB2"}},
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.Interrupted});
}
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});
assert(!res.cursor.firstBatch[3]);

coll.drop();

// Testing ordered:true short circuits.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: doc},
        {insert: 0, document: {_id: 2, skey: "MongoDB2"}},
    ],
    nsInfo: [{ns: "test.coll"}],
    ordered: true
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.Interrupted});
}
assert(!res.cursor.firstBatch[2]);

coll.drop();

// Test that a write can fail part way through a write and the write partially executes.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: 11000});
// Make sure that error extra info was correctly added
assert.docEq(res.cursor.firstBatch[1].keyPattern, {_id: 1});
assert.docEq(res.cursor.firstBatch[1].keyValue, {_id: 1});
assert(!res.cursor.firstBatch[2]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 0);
coll.drop();
coll1.drop();

// Test that we continue processing after an error for ordered:false.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 1, document: {skey: "MongoDB"}}
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.coll1"}],
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

assert(res.cursor.id == 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: 11000});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});
assert(!res.cursor.firstBatch[3]);

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

// Make sure delete multi:true + return fails the op.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {delete: 0, filter: {_id: 1}, multi: true, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[1]);

// Test delete providing returnFields without return option.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 0}, returnFields: {_id: 1}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, idx: 1, code: ErrorCodes.InvalidOptions});
assert(!res.cursor.firstBatch[2]);

coll.drop();

// Test delete fails userAllowedWriteNS.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            delete: 0,
            filter: {_id: 1},
        },
    ],
    nsInfo: [{ns: "test.system.profile"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

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

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

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

assert.commandWorked(res);
assert.eq(res.numErrors, 1);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, code: ErrorCodes.InvalidNamespace});
assert(!res.cursor.firstBatch[1]);

assert.eq(coll.findOne().skey, "MongoDB");

coll.drop();

// Test running multiple findAndModify ops in a command.
// For normal commands this should succeed and for retryable writes the top level should fail.

// Want to make sure both update + delete handle this correctly so test the following combinations
// of ops. update + delete, delete + update. This will prove that both ops set and check the flag
// correctly so doing update + update and delete + delete is redundant.

// update + delete
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "pre"},
        {delete: 0, filter: {_id: 1}, return: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

let processCursor = true;
try {
    assert.commandWorked(res);
    assert.eq(res.numErrors, 0);
} catch {
    processCursor = false;
    assert.commandFailedWithCode(res, [ErrorCodes.BadValue]);
    assert.eq(res.errmsg, "BulkWrite can only support 1 op with a return for a retryable write");
}

if (processCursor) {
    cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
    assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB"});
    cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
    assert.docEq(res.cursor.firstBatch[2].value, {_id: 1, skey: "MongoDB2"});
    assert(!res.cursor.firstBatch[3]);
}

coll.drop();

// delete + update
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB"}},
        {delete: 0, filter: {_id: 2}, return: true},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "pre"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

processCursor = true;
try {
    assert.commandWorked(res);
    assert.eq(res.numErrors, 0);
} catch {
    processCursor = false;
    assert.commandFailedWithCode(res, [ErrorCodes.BadValue]);
    assert.eq(res.errmsg, "BulkWrite can only support 1 op with a return for a retryable write");
}

if (processCursor) {
    cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
    cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
    cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
    assert.docEq(res.cursor.firstBatch[2].value, {_id: 2, skey: "MongoDB"});
    cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, nModified: 1});
    assert.docEq(res.cursor.firstBatch[3].value, {_id: 1, skey: "MongoDB"});
    assert(!res.cursor.firstBatch[4]);
}

coll.drop();

// Test BypassDocumentValidator
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(db.runCommand({collMod: "coll", validator: {a: {$exists: true}}}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 3, skey: "MongoDB"}}],
    nsInfo: [{ns: "test.coll"}],
    bypassDocumentValidation: false,
});
assert.commandWorked(res);
assert.eq(res.numErrors, 1);

assert.eq(0, coll.count({_id: 3}));
coll.drop();

// Test that we correctly count multiple errors for different write types when ordered=false.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1}},
        {insert: 0, document: {_id: 2}},
        // error 1: duplicate key error
        {insert: 0, document: {_id: 1}},
        {delete: 0, filter: {_id: 2}},
        // error 2: user can't write to namespace
        {delete: 1, filter: {_id: 0}},
        {update: 0, filter: {_id: 0}, updateMods: {$set: {x: 1}}},
        // error 3: invalid update operator
        {update: 0, filter: {_id: 0}, updateMods: {$blah: {x: 1}}},
    ],
    nsInfo: [{ns: "test.coll"}, {ns: "test.system.profile"}],
    ordered: false
});

assert.commandWorked(res);
assert.eq(res.numErrors, 3);

coll.drop();
})();
