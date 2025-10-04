// SERVER-14691: $avg aggregator should return null when it receives no input.
let coll = db.accumulate_avg_sum_null;

// Test the $avg aggregator.
coll.drop();

// Null cases.
assert.commandWorked(coll.insert({a: 1, b: 2, c: "string", d: null}));

// Missing field.
let pipeline = [{$group: {_id: "$a", avg: {$avg: "$missing"}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: null}]);

// Non-numeric field.
pipeline = [{$group: {_id: "$a", avg: {$avg: "$c"}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: null}]);

// Field with value of null.
pipeline = [{$group: {_id: "$a", avg: {$avg: "$d"}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: null}]);

// All three.
coll.insert({a: 1, d: "string"});
coll.insert({a: 1});
pipeline = [{$group: {_id: "$a", avg: {$avg: "$d"}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: null}]);

// Non-null cases.
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: 2}));
pipeline = [{$group: {_id: "$a", avg: {$avg: "$b"}}}];

// One field.
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: 2}]);

// Two fields.
assert.commandWorked(coll.insert({a: 1, b: 4}));
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: 3}]);

// Average of zero should still work.
assert.commandWorked(coll.insert({a: 1, b: -6}));
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: 0}]);

// Missing, null, or non-numeric fields should not error or affect the average.
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1, b: "string"}));
assert.commandWorked(coll.insert({a: 1, b: null}));
assert.eq(coll.aggregate(pipeline).toArray(), [{_id: 1, avg: 0}]);
