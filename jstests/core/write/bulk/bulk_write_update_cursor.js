/**
 * Tests bulk write cursor response for correct responses.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: bulkWrite.
 *   not_allowed_with_security_token,
 *   command_not_supported_in_serverless,
 *   # TODO SERVER-52419 Remove this tag.
 *   featureFlagBulkWriteCommand,
 *   # TODO SERVER-79506 Remove this tag.
 *   assumes_unsharded_collection,
 * ]
 */
import {cursorEntryValidator} from "jstests/libs/bulk_write_utils.js";

var coll = db.getCollection("coll");
var coll1 = db.getCollection("coll1");
coll.drop();
coll1.drop();

// Test generic update.
var res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[1].value);
assert(!res.cursor.firstBatch[2]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();
coll1.drop();

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
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[3]);
assert.eq(coll.find({skey: "MongoDB2"}).itcount(), 1);

coll.drop();

// Test Insert outside of bulkWrite + update in bulkWrite.
coll.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

// Test update matches namespace correctly.
coll1.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 0, nModified: 0});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB", coll1.findOne().skey);

coll.drop();
coll1.drop();

// Test Upsert = true (no match so gets inserted)
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {_id: 1});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

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
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {_id: 1});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB", coll.findOne().skey);

coll.drop();

// Test inc operator in updateMods.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, a: 1}},
        {update: 0, filter: {_id: 0}, updateMods: {$inc: {a: 2}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});
assert.eq(res.cursor.firstBatch[1].nModified, 1);
assert(!res.cursor.firstBatch[2]);

coll.drop();

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
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
assert.eq(res.cursor.firstBatch[0].nModified, 1);
assert(!res.cursor.firstBatch[1]);

coll.drop();

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
    nsInfo: [{ns: "test.coll"}],
    let : {targetKey: "MongoDB"}
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[4]);

assert.sameMembers(
    coll.find().toArray(),
    [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

coll.drop();

// Test constants works in pipeline update.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB3"}},
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey"]}},
            updateMods: [{$set: {skey: "$$replacedKey"}}],
            constants: {targetKey: "MongoDB", replacedKey: "MongoDB2"}
        },
    ],
    nsInfo: [{ns: "test.coll"}],
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[4]);

assert.sameMembers(
    coll.find().toArray(),
    [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

coll.drop();

// Test let matches specific document (targetKey) and constants overwrite let (replacedKey).
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB2"}},
        {insert: 0, document: {_id: 2, skey: "MongoDB3"}},
        {
            update: 0,
            filter: {$expr: {$eq: ["$skey", "$$targetKey"]}},
            updateMods: [{$set: {skey: "$$replacedKey"}}],
            constants: {replacedKey: "MongoDB4"}
        },
    ],
    nsInfo: [{ns: "test.coll"}],
    let : {targetKey: "MongoDB3", replacedKey: "MongoDB2"}
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[4]);

coll.drop();

// Test multiple updates on same namespace.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB3"}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1, nModified: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1, nModified: 1});
assert(!res.cursor.firstBatch[3]);

assert.eq("MongoDB3", coll.findOne().skey);

coll.drop();

// Test upsert with implicit collection creation.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, upsert: true},
    ],
    nsInfo: [{ns: "test.coll2"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {_id: 1});
assert(!res.cursor.firstBatch[1]);

var coll2 = db.getCollection("coll2");
coll2.drop();
