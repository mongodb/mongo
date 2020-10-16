// Test the useReferenceIndexForIndexBuild optimization. This test goes through the conditions
// in which a reference index can be used instead of performing a collection scan. for example, a
// reference index cannot be used when the child index (the index we are trying to build) is unique.
(function() {

const conn = MongoRunner.runMongod({setParameter: "useReferenceIndexForIndexBuild=true"});
const db = conn.getDB(jsTestName());

t = db.index_refidx;
t.drop();

// Insert a couple of items
t.insert({a: "a", b: "red", c: 80, d: "asdf"});    // recId 1
t.insert({a: "a", b: "blue", c: 800, d: "asdf"});  // recId 2 - Will be inverted on { a: 1, b: 1 }
t.insert({a: "b", b: "red", c: 80, d: "asdf"});
t.insert({a: "c", b: "blue", c: 800, d: "asdf"});
t.insert({a: "d", b: "red", c: 80, d: "asdf"});
t.insert({a: "a", b: "blue", c: 800, d: "asdf"});
t.insert({a: "a", b: "red", c: 80, d: "asdf"});
t.insert({a: "b", b: "blue", c: 800, d: "asdf"});
t.insert({a: "b", b: "red", c: 80, d: "asdf"});
t.insert({a: "b", b: "blue", c: 800, d: "asdf"});
t.insert({a: "a", b: "red", c: 80, d: "asdf"});
t.insert({a: "c", b: "blue", c: 800, d: "asdf"});

assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_xyz_1"));  // should take the normal path
assert.eq(5, t.find({a: "a"}).count());                             // should be 5

// Ensure key class sorts are working properly (inverted keys were inserted earlier).
assert.commandWorked(t.createIndex({a: 1}));  // should take fast path
assert(checkLog.checkContainsOnceJson(
    conn, 3620203, {refIdx: "refidx_xyz_1"}));  // checkContainsOnceJson

assert.eq(5, t.find({a: "a"}).hint({a: 1}).count());
assert.eq(4, t.find({a: "b"}).hint({a: 1}).count());
assert.eq(0, t.find({a: "aa"}).hint({a: 1}).count());

let result = assert.commandWorked(t.validate());
assert(result.valid);

t.drop();

// Insert all unique keys
for (var i = 0; i < 30; i++) {
    t.insert({a: i, b: (i * 10)});
    t.insert({a: (-i), b: (i * -10)});
}

assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_xyz_2"));
assert.commandWorked(t.createIndex({a: 1}));
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_2"}));
assert.eq(29, t.find({a: {$gt: 0}}).hint({a: 1}).count());

assert.commandWorked(t.createIndex({a: -1}));  // Shouldn't take the fast path.
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_2"}));
assert.eq(0, t.find({a: "blah"}).hint({a: 1}).count());  // None should be found.

// No problems inserting new documents into the existing index.
for (var i = 0; i < 40; i++) {
    t.insert({a: i, b: (i + 1)});
}

assert.eq(68, t.find({a: {$gt: 0}}).hint({a: 1}).count());

result = assert.commandWorked(t.validate());
assert(result.valid);
t.drop();

assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_xyz_3"));

// This test case covers a key class distribution of the form
// [ a a ... a b b b b ...b c d e f g g g g ..g].
// We are trying to make sure documents sitting on key class boundaries are not skipped.
for (var i = 0; i < 20; i++) {
    t.insert({a: "a", b: i});
    t.insert({a: "b", b: (2 * i)});
    t.insert({a: "b", b: i});
    t.insert({a: "g", b: (i - 1)});
}

t.insert({a: "c", b: 2});
t.insert({a: "f", b: 2});
t.insert({a: "e", b: 2});
t.insert({a: "d", b: 2});

assert.commandWorked(t.createIndex({b: 1, a: 1}, "refidx_xyz_4"));
assert.commandWorked(t.createIndex({a: 1}));
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_3"}));
assert.eq(20, t.find({a: "a"}).hint({a: 1}).count());
assert.eq(40, t.find({a: "b"}).hint({a: 1}).count());
assert.eq(20, t.find({a: "g"}).hint({a: 1}).count());
assert.eq(1, t.find({a: "c"}).hint({a: 1}).count());
assert.eq(1, t.find({a: "d"}).hint({a: 1}).count());
assert.eq(1, t.find({a: "e"}).hint({a: 1}).count());
assert.eq(1, t.find({a: "f"}).hint({a: 1}).count());
assert.eq(0, t.find({a: "h"}).hint({a: 1}).count());

assert.commandWorked(
    t.createIndex({a: 1, b: 1, c: 1}, "refidx_xyz_5"));  // shouldn't take the fast path
assert.commandWorked(t.createIndex({a: 1, c: 1}));       // shouldn't take the fast path
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_3"}));

t.dropIndex({a: 1, b: 1});
assert.commandWorked(t.createIndex({a: 1, b: 1}));  // should take the fast path
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_5"}));
assert.commandWorked(t.createIndex({b: 1}));  // should take the fast path
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_4"}));

result = assert.commandWorked(t.validate());
assert(result.valid);

t.drop();

// An empty index:
assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_xyz_6"));
assert.commandWorked(t.createIndex({a: 1}));
// Empty collections are handled elsewhere.
// assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_6"}));
assert.eq(0, t.find().hint({a: 1}).count());
assert.eq(0, t.find().count());

result = assert.commandWorked(t.validate());
assert(result.valid);

t.drop();

// An index with one element
assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_xyz_7"));
t.insert({a: "we are the robots", b: "kraftwerk"});
assert.commandWorked(t.createIndex({a: 1}));
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_7"}));
assert.eq(0, t.find({a: "spacelab"}).hint({a: 1}).count());
assert.eq(1, t.find({a: "we are the robots"}).hint({a: 1}).count());

result = assert.commandWorked(t.validate());
assert(result.valid);
t.drop();

// Parent can be unique, and we can build a non-unique child from it. Check partial indexes as well.
for (var i = 0; i < 30; i++) {
    t.insert({a: 1, b: i});
}

assert.commandWorked(t.createIndex({a: 1, b: 1}, {unique: true, name: "refidx_uniq_a_b"}));
assert.commandWorked(t.createIndex({b: 1, a: 1}, {unique: true, name: "refidx_uniq_b_a"}));
assert.commandWorked(t.createIndex({a: 1}, "childidx_a1"));
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_uniq_a_b"}));
assert.commandWorked(t.createIndex({b: 1}, {unique: true}));  // shouldn't use the fast path.
assert(!checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_xyz_b_a"}));
assert.commandWorked(t.dropIndex("childidx_a1"));
assert.commandWorked(t.dropIndex("refidx_uniq_a_b"));
assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_partial_a_b"));
assert.commandWorked(t.createIndex({a: 1}, {partialFilterExpression: {b: {$gt: 5}}}));
assert(!checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_partial_a_b"}));

result = assert.commandWorked(t.validate());
assert(result.valid);
t.drop();

// Make sure we're using the smallest available reference index.
for (var i = 0; i < 30; i++) {
    t.insert({a: 1, b: i, c: (i + 1), d: (i * 3)});
}

assert.commandWorked(t.createIndex({a: 1, b: 1, c: 1, d: 1}, "refidx_abcd_xyz"));
assert.commandWorked(t.createIndex({a: 1, b: 1}, "refidx_ab_xyz"));
assert(checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "refidx_abcd_xyz"}));
assert.commandWorked(t.createIndex({a: 1, b: 1, c: 1}, "refidx_abc_xyz"));
assert.commandWorked(t.createIndex({a: 1, c: 1}, "refidx_ac_xyz"));
assert.commandWorked(t.createIndex({a: 1}, "childidx_a_smallest"));
assert(checkLog.checkContainsOnceJson(
    conn, 3620203, {childIdx: "childidx_a_smallest", refIdx: "refidx_ab_xyz"}));

result = assert.commandWorked(t.validate());
assert(result.valid);
t.drop();

// Make sure multi-key isn't permitted
t.insert({a: ["me", "mee", "and louie"], b: 78});
t.insert({a: ["summer", "is a good season"], b: 8});
assert.commandWorked(t.createIndex({a: 1, b: 1}, "multikey_ab_xyz"));
assert.commandWorked(t.createIndex({a: 1}, "child_shouldn_use_ref"));
assert(!checkLog.checkContainsOnceJson(conn, 3620203, {refIdx: "multikey_ab_xyz"}));

result = assert.commandWorked(t.validate());
assert(result.valid);
t.drop();

MongoRunner.stopMongod(conn);
})();