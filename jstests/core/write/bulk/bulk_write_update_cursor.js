/**
 * Tests bulk write cursor response for correct responses.
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

const cursorEntryValidator = function(entry, expectedEntry) {
    assert(entry.ok == expectedEntry.ok);
    assert(entry.idx == expectedEntry.idx);
    assert(entry.n == expectedEntry.n);
    assert(entry.nModified == expectedEntry.nModified);
    assert(entry.code == expectedEntry.code);
};

// Test generic update with no return.
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
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
assert(!res.cursor.firstBatch[1].value);
assert(!res.cursor.firstBatch[2]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();
coll1.drop();

// Test return:pre.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "pre"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB"});
assert(!res.cursor.firstBatch[2]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();
coll1.drop();

// Test return:post.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "post"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[2]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

// Test only updates one when multi is false (default value) with sort.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {
            update: 0,
            filter: {skey: "MongoDB"},
            updateMods: {$set: {skey: "MongoDB2"}},
            sort: {_id: -1},
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, nModified: 1});
assert.docEq(res.cursor.firstBatch[2].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[3]);
assert.sameMembers(coll.find().toArray(), [{_id: 0, skey: "MongoDB"}, {_id: 1, skey: "MongoDB2"}]);

coll.drop();

// Test only updates one when multi is false (default value) with sort.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, skey: "MongoDB"}},
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {
            update: 0,
            filter: {skey: "MongoDB"},
            updateMods: {$set: {skey: "MongoDB2"}},
            sort: {_id: 1},
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, nModified: 1});
assert.docEq(res.cursor.firstBatch[2].value, {_id: 0, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[3]);
assert.sameMembers(coll.find().toArray(), [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB"}]);

coll.drop();

// Test Insert outside of bulkWrite + update in bulkWrite.
coll.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "post"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 1});
assert.docEq(res.cursor.firstBatch[0].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

// Test update matches namespace correctly.
coll1.insert({_id: 1, skey: "MongoDB"});

res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "post"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 0});
assert(!res.cursor.firstBatch[0].value);
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB", coll1.findOne().skey);

coll.drop();
coll1.drop();

// Test Upsert = true (no match so gets inserted). With return pre no value should be returned.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            upsert: true,
            return: "pre"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {index: 0, _id: 1});
assert(!res.cursor.firstBatch[0].value);
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

// Test Upsert = true (no match so gets inserted). With return post the document should be returned.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            upsert: true,
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {index: 0, _id: 1});
assert.docEq(res.cursor.firstBatch[0].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[1]);

assert.eq("MongoDB2", coll.findOne().skey);

coll.drop();

// Test inc operator in updateMods.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 0, a: 1}},
        {update: 0, filter: {_id: 0}, updateMods: {$inc: {a: 2}}, return: "post"},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 0, a: 3});
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
            arrayFilters: [{"i.b": 5}],
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 1});
assert.eq(res.cursor.firstBatch[0].nModified, 1);
assert.docEq(res.cursor.firstBatch[0].value, {_id: 0, a: [{b: 6}, {b: 1}, {b: 2}]});
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
            updateMods: {skey: "MongoDB2"},
            let : {targetKey: "MongoDB"},
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, n: 1});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, n: 1});
cursorEntryValidator(res.cursor.firstBatch[3], {ok: 1, idx: 3, nModified: 1});
assert.docEq(res.cursor.firstBatch[3].value, {_id: 0, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[4]);

assert.sameMembers(
    coll.find().toArray(),
    [{_id: 0, skey: "MongoDB2"}, {_id: 1, skey: "MongoDB2"}, {_id: 2, skey: "MongoDB3"}]);

coll.drop();

// Test multiple updates on same namespace.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, skey: "MongoDB"}},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB2"}}, return: "post"},
        {update: 0, filter: {_id: 1}, updateMods: {$set: {skey: "MongoDB3"}}},
    ],
    nsInfo: [{ns: "test.coll"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, idx: 1, nModified: 1});
assert.docEq(res.cursor.firstBatch[1].value, {_id: 1, skey: "MongoDB2"});
cursorEntryValidator(res.cursor.firstBatch[2], {ok: 1, idx: 2, nModified: 1});
assert(!res.cursor.firstBatch[3]);

assert.eq("MongoDB3", coll.findOne().skey);

coll.drop();

// Test upsert with implicit collection creation.
res = db.adminCommand({
    bulkWrite: 1,
    ops: [
        {
            update: 0,
            filter: {_id: 1},
            updateMods: {$set: {skey: "MongoDB2"}},
            upsert: true,
            return: "post"
        },
    ],
    nsInfo: [{ns: "test.coll2"}]
});

assert.commandWorked(res);
assert.eq(res.numErrors, 0);

cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, nModified: 0});
assert.docEq(res.cursor.firstBatch[0].upserted, {index: 0, _id: 1});
assert.docEq(res.cursor.firstBatch[0].value, {_id: 1, skey: "MongoDB2"});
assert(!res.cursor.firstBatch[1]);

var coll2 = db.getCollection("coll2");
coll2.drop();
})();
