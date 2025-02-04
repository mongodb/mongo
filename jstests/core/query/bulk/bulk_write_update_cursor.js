/**
 * Tests bulk write cursor response for correct responses.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_signed_security_token,
 *   command_not_supported_in_serverless,
 *   requires_fcv_80
 * ]
 */
import {
    cursorEntryValidator,
    cursorSizeValidator,
    summaryFieldsValidator
} from "jstests/libs/bulk_write_utils.js";

const coll = db[jsTestName()];
const collName = coll.getFullName();
const coll1 = db[jsTestName() + "1"];
coll.drop();
coll1.drop();

// Test generic update.
let res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 1, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});

assert.eq("MongoDB2", coll.findOne().skey);

assert(coll.drop());
assert(coll1.drop());

// Test only updates one when multi is false (default value).
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {
            update: 0,
            filter: {skey: "MongoDB"},
            updateMods: {$set: {skey: "MongoDB2"}},
        },
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});
assert.eq(coll.find({skey: "MongoDB2"}).itcount(), 1);

assert(coll.drop());

// Test Insert outside of bulkWrite + update in bulkWrite.
assert.commandWorked(coll.insert({_id: 1, skey: "MongoDB"}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});

assert.eq("MongoDB2", coll.findOne().skey);

assert(coll.drop());

// Test update matches namespace correctly.
assert.commandWorked(coll1.insert({_id: 1, skey: "MongoDB"}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 0, nModified: 0});

assert.eq("MongoDB", coll1.findOne().skey);

assert(coll.drop());
assert(coll1.drop());

// Test Upsert = true (no match so gets inserted)
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});

cursorEntryValidator(res.cursor.firstBatch[0],
                     {ok: 1, idx: 0, n: 1, nModified: 0, upserted: {_id: 1}});

assert.eq("MongoDB2", coll.findOne().skey);

assert(coll.drop());

// Test Upsert = True with UpsertSupplied = True (no match so insert constants.new)
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: [{$set: {skey: "MongoDB2"}}],
            upsert: true,
            upsertSupplied: true,
            constants: {new: {skey: "MongoDB"}},
        },
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});

cursorEntryValidator(res.cursor.firstBatch[0],
                     {ok: 1, idx: 0, n: 1, nModified: 0, upserted: {_id: 1}});

assert.eq("MongoDB", coll.findOne().skey);

assert(coll.drop());

// Test inc operator in updateMods.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, a: 1}},
        {update: 0, filter: {_id: 0}, updateMods: {$inc: {a: 2}}},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 2);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 1, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});

assert(coll.drop());

// Test arrayFilters matches specific array element.
assert.commandWorked(coll.insert({_id: 0, a: [{b: 5}, {b: 1}, {b: 2}]}));

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 0},
            updateMods: {$set: {"a.$[i].b": 6}},
            arrayFilters: [{"i.b": 5}]
        },
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});

assert(coll.drop());

// Test let matches specific document.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB3"}},
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey"]}},
            updateMods: {skey: "MongoDB2"}
        },
    ],
    nsInfo: [{ns: collName}],
    let : {targetKey: "MongoDB"}
});

assert.commandWorked(res);
cursorSizeValidator(res, 4);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 3, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});

assert.sameMembers(
    coll.find().toArray(),
    [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

assert(coll.drop());

// Test multiple updates on same namespace.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB3"}}},
    ],
    nsInfo: [{ns: collName}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 3);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 1, nDeleted: 0, nMatched: 2, nModified: 2, nUpserted: 0});

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});

assert.eq("MongoDB3", coll.findOne().skey);

assert(coll.drop());

const coll2 = db[jsTestName() + "2"];
coll2.drop();

// Test upsert with implicit collection creation.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
    ],
    nsInfo: [{ns: "test.coll2"}]
});

assert.commandWorked(res);
cursorSizeValidator(res, 1);
summaryFieldsValidator(
    res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});

cursorEntryValidator(res.cursor.firstBatch[0],
                     {ok: 1, idx: 0, n: 1, nModified: 0, upserted: {_id: 1}});

assert(coll2.drop());
