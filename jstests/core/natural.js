// Tests for $natural sort and $natural hint.
(function() {
'use strict';

var results;

var coll = db.jstests_natural;
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 3}));
assert.commandWorked(coll.insert({_id: 2, a: 2}));
assert.commandWorked(coll.insert({_id: 3, a: 1}));

// Regression test for SERVER-20660. Ensures that documents returned with $natural don't have
// any extraneous fields.
results = coll.find({a: 2}).sort({$natural: 1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0], {_id: 2, a: 2});

// Regression test for SERVER-20660. Ensures that documents returned with $natural don't have
// any extraneous fields.
results = coll.find({a: 2}).hint({$natural: -1}).toArray();
assert.eq(results.length, 1);
assert.eq(results[0], {_id: 2, a: 2});

// $natural hint with non-$natural sort is allowed.
assert.eq([{_id: 3, a: 1}, {_id: 2, a: 2}, {_id: 1, a: 3}],
          coll.find().hint({$natural: 1}).sort({a: 1}).toArray());

// $natural sort with non-$natural hint is not allowed.
assert.throws(() => coll.find().hint({a: 1}).sort({$natural: 1}).itcount());

// Test that a compound $natural hint is not allowed.
assert.throws(() => coll.find().hint({a: 1, $natural: 1}).itcount());
assert.throws(() => coll.find().hint({$natural: 1, b: 1}).itcount());
assert.throws(() => coll.find().hint({a: 1, $natural: 1, b: 1}).itcount());

// Test that a compound $natural sort is not allowed.
assert.throws(() => coll.find().sort({a: 1, $natural: 1}).itcount());
assert.throws(() => coll.find().sort({$natural: 1, b: 1}).itcount());
assert.throws(() => coll.find().sort({a: 1, $natural: 1, b: 1}).itcount());
})();
