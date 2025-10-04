// @tags: [
//   requires_capped,
//   requires_getmore,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   # This test relies on query commands returning specific batch-sized responses.
//   assumes_no_implicit_cursor_exhaustion,
// ]

// Test that tailable cursors work correctly with skip and limit.
// Setup the capped collection.
let collname = "jstests_tailable_skip_limit";
let t = db[collname];
t.drop();
assert.commandWorked(db.createCollection(collname, {capped: true, size: 1024}));

assert.commandWorked(t.insert({_id: 1}));
assert.commandWorked(t.insert({_id: 2}));

// Non-tailable with skip
var cursor = t.find().skip(1);
assert.eq(2, cursor.next()["_id"]);
assert(!cursor.hasNext());
assert.commandWorked(t.insert({_id: 3}));
assert(!cursor.hasNext());

// Non-tailable with limit
var cursor = t.find().limit(100);
for (var i = 1; i <= 3; i++) {
    assert.eq(i, cursor.next()["_id"]);
}
assert(!cursor.hasNext());
assert.commandWorked(t.insert({_id: 4}));
assert(!cursor.hasNext());

// Non-tailable with negative limit
var cursor = t.find().batchSize(100).limit(-100);
for (var i = 1; i <= 4; i++) {
    assert.eq(i, cursor.next()["_id"]);
}
assert(!cursor.hasNext());
assert.commandWorked(t.insert({_id: 5}));
assert(!cursor.hasNext());

// Tailable with skip
cursor = t.find().addOption(2).skip(4);
assert.eq(5, cursor.next()["_id"]);
assert(!cursor.hasNext());
assert.commandWorked(t.insert({_id: 6}));
assert(cursor.hasNext());
assert.eq(6, cursor.next()["_id"]);

// Tailable with limit
var cursor = t.find().addOption(2).limit(100);
for (var i = 1; i <= 6; i++) {
    assert.eq(i, cursor.next()["_id"]);
}
assert(!cursor.hasNext());
assert.commandWorked(t.insert({_id: 7}));
assert(cursor.hasNext());
assert.eq(7, cursor.next()["_id"]);

// Tailable with negative limit is an error.
assert.throws(function () {
    t.find().addOption(2).limit(-100).next();
});
assert.throws(function () {
    t.find().addOption(2).limit(-1).itcount();
});

// A limit of 1 with the tailable option is allowed.
assert.eq(1, t.find().addOption(2).limit(1).itcount());

// Tests that a tailable cursor over an empty capped collection produces a dead cursor, intended
// to be run on both mongod and mongos. For SERVER-20720.
t.drop();
assert.commandWorked(db.createCollection(t.getName(), {capped: true, size: 1024}));

let cmdRes = db.runCommand({find: t.getName(), tailable: true});
assert.commandWorked(cmdRes);
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, t.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 0);

// Test that the cursor works in the shell.
assert.eq(t.find().addOption(2).itcount(), 0);
assert.commandWorked(t.insert({a: 1}));
assert.eq(t.find().addOption(2).itcount(), 1);
