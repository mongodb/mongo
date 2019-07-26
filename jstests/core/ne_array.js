// This is a test for the query correctness bug described in SERVER-38949. A {$ne: <array>} query
// cannot "naively" use an index. That is, it cannot use an index by simply generating bounds for
// {$eq: <array>} query and then complementing them. This test checks that the correct results are
// returned for this type of query when an index is present.
// @tags: [requires_non_retryable_writes]
(function() {
const coll = db.ne_array;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insert({_id: 0, a: [1]}));
assert.commandWorked(coll.insert({_id: 1, a: [1, 3]}));

assert.eq(coll.find({a: {$ne: [1, 3]}}, {_id: 1}).toArray(), [{_id: 0}]);
assert.eq(coll.find({a: {$ne: [1]}}, {_id: 1}).toArray(), [{_id: 1}]);

assert.eq(coll.find({a: {$not: {$in: [[1]]}}}, {_id: 1}).toArray(), [{_id: 1}]);
assert.eq(coll.find({a: {$not: {$in: [[1, 3]]}}}, {_id: 1}).toArray(), [{_id: 0}]);
assert.eq(coll.find({a: {$not: {$in: [[1], [1, 3]]}}}, {_id: 1}).toArray(), []);
assert.eq(coll.find({a: {$not: {$in: ["scalar value", [1, 3]]}}}, {_id: 1}).toArray(), [{_id: 0}]);

// Insert some documents which have nested arrays so we can test $elemMatch value.
assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: [[123]]}));
assert.commandWorked(coll.insert({_id: 1, a: [4, 5, [123]]}));
assert.commandWorked(coll.insert({_id: 2, a: [7, 8]}));

// sort by _id in case we run on a sharded cluster which puts the documents on different
// shards (and thus, might return them in any order).
assert.eq(coll.find({a: {$elemMatch: {$not: {$eq: [123]}}}}, {_id: 1}).sort({_id: 1}).toArray(),
          [{_id: 1}, {_id: 2}]);

assert.eq(coll.find({a: {$elemMatch: {$not: {$in: [[123]]}}}}, {_id: 1}).sort({_id: 1}).toArray(),
          [{_id: 1}, {_id: 2}]);

assert.eq(coll.find({a: {$not: {$elemMatch: {$eq: [123]}}}}, {_id: 1}).toArray(), [{_id: 2}]);
assert.eq(coll.find({a: {$not: {$elemMatch: {$in: [[123]]}}}}, {_id: 1}).toArray(), [{_id: 2}]);

// Test $elemMatch object.
assert.commandWorked(coll.remove({}));
coll.dropIndexes();
assert.commandWorked(coll.createIndex({"a.b": 1}));
assert.commandWorked(coll.insert({_id: 0, a: [[123]]}));
assert.commandWorked(coll.insert({_id: 1, a: [{b: 123}]}));
assert.commandWorked(coll.insert({_id: 2, a: [{b: [4, [123]]}]}));
assert.commandWorked(coll.insert({_id: 3, a: [{b: [[123]]}]}));

// Remember that $ne with an array will match arrays where _none_ of the elements match.
assert.eq(coll.find({a: {$elemMatch: {b: {$ne: [123]}}}}, {_id: 1}).sort({_id: 1}).toArray(),
          [{_id: 0}, {_id: 1}]);
assert.eq(
    coll.find({a: {$elemMatch: {b: {$not: {$in: [[123]]}}}}}, {_id: 1}).sort({_id: 1}).toArray(),
    [{_id: 0}, {_id: 1}]);

assert.eq(coll.find({a: {$not: {$elemMatch: {b: [123]}}}}, {_id: 1}).sort({_id: 1}).toArray(),
          [{_id: 0}, {_id: 1}]);
assert.eq(
    coll.find({a: {$not: {$elemMatch: {b: {$in: [[123]]}}}}}, {_id: 1}).sort({_id: 1}).toArray(),
    [{_id: 0}, {_id: 1}]);
})();
