/**
 * Tests passing hint to the count command:
 *   - A hint should be respected even if it results in an incorrect count.
 *   - A bad argument to the hint() method should raise an error.
 *   - The hint() method should support both the name of the index, and the object spec of the
 *     index.
 *
 * @tags: [requires_fastcount]
 */
let coll = db.jstests_count_hint;
coll.drop();

assert.commandWorked(coll.insert({i: 1}));
assert.commandWorked(coll.insert({i: 2}));

assert.eq(2, coll.find().count());

assert.commandWorked(coll.createIndex({i: 1}));

assert.eq(2, coll.find().hint("i_1").count());
assert.eq(2, coll.find().hint({i: 1}).count());

assert.eq(1, coll.find({i: 1}).hint("_id_").count());
assert.eq(1, coll.find({i: 1}).hint({_id: 1}).count());

assert.eq(2, coll.find().hint("_id_").count());
assert.eq(2, coll.find().hint({_id: 1}).count());

// Create a sparse index which should have no entries.
assert.commandWorked(coll.createIndex({x: 1}, {sparse: true}));

// A hint should be respected, even if it results in the wrong answer.
assert.eq(0, coll.find().hint("x_1").count());
assert.eq(0, coll.find().hint({x: 1}).count());

assert.eq(0, coll.find({i: 1}).hint("x_1").count());
assert.eq(0, coll.find({i: 1}).hint({x: 1}).count());

// SERVER-14792: bad hints should cause the count to fail, even if there is no query predicate.
assert.throws(function () {
    coll.find().hint({bad: 1, hint: 1}).count();
});
assert.throws(function () {
    coll.find({i: 1}).hint({bad: 1, hint: 1}).count();
});

assert.throws(function () {
    coll.find().hint("BAD HINT").count();
});
assert.throws(function () {
    coll.find({i: 1}).hint("BAD HINT").count();
});

// Test that a bad hint fails with the correct error code.
let cmdRes = db.runCommand({count: coll.getName(), hint: {bad: 1, hint: 1}});
assert.commandFailedWithCode(cmdRes, ErrorCodes.BadValue, tojson(cmdRes));
let regex = new RegExp("hint provided does not correspond to an existing index");
assert(regex.test(cmdRes.errmsg));
