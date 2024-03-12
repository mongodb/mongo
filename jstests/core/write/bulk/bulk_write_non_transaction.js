/**
 * Tests bulk write command for scenarios that cause the command to fail (ok: 0).
 *
 * These tests are incompatible with the transaction overrides since any failure
 * will cause a transaction abortion which will make the overrides infinite loop.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   command_not_supported_in_serverless,
 *   # Contains commands that fail which will fail the entire transaction
 *   does_not_support_transactions,
 *   operations_longer_than_stepdown_interval,
 *   requires_fcv_80
 * ]
 */
import {
    cursorEntryValidator,
    cursorSizeValidator,
    summaryFieldsValidator
} from "jstests/libs/bulk_write_utils.js";

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

var maxWriteBatchSize = db.hello().maxWriteBatchSize;
var insertOp = {insert: 0, document: {_id: 1, skey: "MongoDB"}};

// Make sure bulkWrite at maxWriteBatchSize is okay
let ops = [];
for (var i = 0; i < maxWriteBatchSize; ++i) {
    ops.push(insertOp);
}

var res = db.adminCommand({
    bulkWrite: 1,
    ops: ops,
    nsInfo: [{ns: "test.coll"}],
});

// It is also possible to see interruption here due to very large batch size.
if (!ErrorCodes.isInterruption(res.code)) {
    assert.commandWorked(res);
}
coll.drop();

// Make sure bulkWrite above maxWriteBatchSize fails
ops = [];
for (var i = 0; i < maxWriteBatchSize + 1; ++i) {
    ops.push(insertOp);
}

res = db.adminCommand({
    bulkWrite: 1,
    ops: ops,
    nsInfo: [{ns: "test.coll"}],
});

// It is also possible to see interruption here due to very large batch size.
if (!ErrorCodes.isInterruption(res.code)) {
    assert.commandFailedWithCode(res, [ErrorCodes.InvalidLength]);
}

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
assert.commandFailedWithCode(db.adminCommand({bulkWrite: 1, nsInfo: [{ns: "mydb.coll"}]}),
                             [ErrorCodes.IDLFailedToParse]);

assert.eq(coll.find().itcount(), 0);
assert.eq(coll1.find().itcount(), 0);

// Missing nsInfo
assert.commandFailedWithCode(
    db.adminCommand({bulkWrite: 1, ops: [{insert: 0, document: {skey: "MongoDB"}}]}),
    [ErrorCodes.IDLFailedToParse]);

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

var coll2 = db.getCollection("coll2");
coll2.drop();

// Test update continues on error with ordered:false.
assert.commandWorked(coll2.insert({_id: 3}));
assert.commandWorked(coll2.insert({_id: 4}));
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 3}, updateMods: {$inc: {_id: 1}}, upsert: true},
        {update: 1, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
    ],
    nsInfo: [{ns: "test.coll2"}, {ns: "test.coll"}],
    ordered: false
});

assert.commandWorked(res);
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});

cursorEntryValidator(res.cursor.firstBatch[0],
                     {ok: 0, idx: 0, n: 0, nModified: 0, code: ErrorCodes.ImmutableField});
cursorEntryValidator(res.cursor.firstBatch[1],
                     {ok: 1, idx: 1, n: 1, nModified: 0, upserted: {_id: 1}});
coll.drop();
coll2.drop();

// Test update stop on error with ordered:true.
assert.commandWorked(coll2.insert({_id: 3}));
assert.commandWorked(coll2.insert({_id: 4}));
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 3}, updateMods: {$inc: {_id: 1}}, upsert: true},
        {update: 1, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
    ],
    nsInfo: [{ns: "test.coll2"}, {ns: "test.coll"}],
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0],
                     {ok: 0, idx: 0, n: 0, nModified: 0, code: ErrorCodes.ImmutableField});
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
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert.eq(res.cursor.id, 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1],
                         {ok: 0, n: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1],
                         {ok: 0, n: 0, idx: 1, code: ErrorCodes.Interrupted});
}
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});

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
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 1, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert.eq(res.cursor.id, 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});

// In most cases we expect this to fail because it tries to insert a document that is too large.
// In some cases we may see the javascript execution interrupted because it takes longer than
// our default time limit, so we allow that possibility.
try {
    cursorEntryValidator(res.cursor.firstBatch[1],
                         {ok: 0, n: 0, idx: 1, code: ErrorCodes.BadValue});
} catch {
    cursorEntryValidator(res.cursor.firstBatch[1],
                         {ok: 0, n: 0, idx: 1, code: ErrorCodes.Interrupted});
}

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
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 1, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert.eq(res.cursor.id, 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, n: 0, idx: 1, code: 11000});
// Make sure that error extra info was correctly added
assert.docEq(res.cursor.firstBatch[1].keyPattern, {_id: 1});
assert.docEq(res.cursor.firstBatch[1].keyValue, {_id: 1});

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
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert.eq(res.cursor.id, 0);
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 0, n: 0, idx: 1, code: 11000});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, n: 1, idx: 2});

assert.eq(coll.find().itcount(), 1);
assert.eq(coll1.find().itcount(), 1);
coll.drop();
coll1.drop();

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
assert.eq(res.nErrors, 1, "bulkWrite command response: " + tojson(res));

assert.eq(0, coll.count({_id: 3}));
coll.drop();

// Checking n and nModified on update success and failure.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {_id: 2}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 1, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[2],
                     {ok: 0, idx: 2, n: 0, nModified: 0, code: ErrorCodes.ImmutableField});
coll.drop();

coll.insert({skey: "MongoDB"});
// Test constants is not supported on non-pipeline update.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "MongoDB"]}},
            updateMods: {skey: "$$targetKey"},
            constants: {targetKey: "MongoDB2"}
        },
    ],
    nsInfo: [{ns: "test.coll"}],
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, n: 0, nModified: 0, code: 51198});
assert.eq(res.cursor.firstBatch[0].errmsg,
          "Constant values may only be specified for pipeline updates");

coll.drop();

// Test Upsert = True with UpsertSupplied = True (no match and constants.new is missing)
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: [{$set: {skey: "MongoDB2"}}],
            upsert: true,
            upsertSupplied: true,
            constants: {},
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, idx: 0, n: 0, nModified: 0, code: 9});
assert.eq(res.cursor.firstBatch[0].errmsg,
          "the parameter 'upsertSupplied' is set to 'true', but no document was supplied");

coll.drop();

// Test errorsOnly with a failure.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 1}}, {insert: 0, document: {_id: 1}}],
    nsInfo: [{ns: "test.coll"}],
    errorsOnly: true
});

assert.commandWorked(res, "bulkWrite command response: " + tojson(res));
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 1, nInserted: 1, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

assert(res.cursor.id == 0, "bulkWrite command response: " + tojson(res));
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 0, n: 0, idx: 1, code: 11000});
assert(!res.cursor.firstBatch[1], "bulkWrite command response: " + tojson(res));
assert.eq(res.cursor.ns, "admin.$cmd.bulkWrite", "bulkWrite command response: " + tojson(res));

coll.drop();

// Explained upsert against an empty collection should succeed and be a no-op.
res = db.adminCommand({
    explain: {
        bulkWrite: 1,
        ops: [
            {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
        ],
        nsInfo: [{ns: "test.coll"}],
        ordered: false
    }
});
assert.commandWorked(res);

// Collection should still not exist.
assert.eq(coll.find().itcount(), 0);

assert.eq(res.executionStats.totalDocsExamined, 0, "bulkWrite explain response: " + tojson(res));

coll.drop();

// Explained update should succeed and be a no-op.
coll.insert({_id: 1, skey: "MongoDB"});
res = db.adminCommand({
    explain: {
        bulkWrite: 1,
        ops: [
            {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
        ],
        nsInfo: [{ns: "test.coll"}],
        ordered: false
    }
});
assert.commandWorked(res);

assert.eq(coll.find({skey: "MongoDB"}).itcount(), 1);

assert.eq(res.executionStats.totalDocsExamined, 1, "bulkWrite explain response: " + tojson(res));

coll.drop();

// Explained delete should succeed and be a no-op.
coll.insert({_id: 1});
res = db.adminCommand({
    explain: {
        bulkWrite: 1,
        ops: [
            {delete: 0, filter: {_id: 1}},
        ],
        nsInfo: [{ns: "test.coll"}],
        ordered: false
    }
});
assert.commandWorked(res);

assert.eq(coll.find().itcount(), 1);

assert.eq(res.executionStats.totalDocsExamined, 1, "bulkWrite explain response: " + tojson(res));

coll.drop();

coll.insert({_id: 1, skey: "MongoDB"});
coll.insert({_id: 2, skey: "MongoDB"});
res = db.adminCommand({
    explain: {
        bulkWrite: 1,
        ops: [
            {delete: 0, filter: {skey: "MongoDB"}, multi: true},
        ],
        nsInfo: [{ns: "test.coll"}],
        ordered: false
    }
});
assert.commandWorked(res);

assert.eq(coll.find().itcount(), 2);

assert.eq(res.executionStats.totalDocsExamined, 2, "bulkWrite explain response: " + tojson(res));

coll.drop();
