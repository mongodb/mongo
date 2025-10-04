// @tags: [
//   assumes_read_concern_local,
//   requires_getmore
// ]

let t = db.array_match4;

t.drop();
t.save({a: [1, 2]});

let query_gte = {a: {$gte: [1, 2]}};

//
// without index
//

assert.eq(1, t.find({a: [1, 2]}).count(), "$eq (without index)");
assert.eq(1, t.find(query_gte).itcount(), "$gte (without index)");

//
// with index
//

t.createIndex({a: 1});
assert.eq(1, t.find({a: [1, 2]}).count(), "$eq (with index)");

// display explain output (for index bounds)
let explain = t.find(query_gte).explain();
print("explain for " + tojson(query_gte, "", true) + " = " + tojson(explain));

// number of documents returned by indexes query should be consistent
// with non-indexed case.
assert.eq(1, t.find(query_gte).itcount(), "$gte (with index)");
