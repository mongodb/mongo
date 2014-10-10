// Check that we return the correct results for negations over a
// multikey index.

var t = db.jstests_not3;
t.drop();

t.ensureIndex({arr: 1});
t.save({_id: 0, arr: [1, 2, 3]});
t.save({_id: 1, arr: [10, 11]});

// Case 1: simple $ne over array field.
var case1 = {arr: {$ne: 3}};
assert.eq(1, t.find(case1).itcount(), "Case 1: wrong number of results");
assert.eq(1, t.findOne(case1)._id, "Case 1: wrong _id");

// Case 2: simple $not over array field.
var case2 = {arr: {$not: {$gt: 6}}};
assert.eq(1, t.find(case2).itcount(), "Case 2: wrong number of results");
assert.eq(0, t.findOne(case2)._id, "Case 2: wrong _id");
