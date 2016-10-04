var t = db.hashindex1;
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

// test non-single field hashed indexes don't get created (maybe change later)
var badspec = {a: "hashed", b: 1};
t.ensureIndex(badspec);
assert.eq(t.getIndexes().length, 1, "only _id index should be created");

// test unique index not created (maybe change later)
var goodspec = {a: "hashed"};
t.ensureIndex(goodspec, {"unique": true});
assert.eq(t.getIndexes().length, 1, "unique index got created.");

// now test that non-unique index does get created
t.ensureIndex(goodspec);
assert.eq(t.getIndexes().length, 2, "hashed index didn't get created");

// test basic inserts
for (i = 0; i < 10; i++) {
    t.insert({a: i});
}
assert.eq(t.find().count(), 10, "basic insert didn't work");
assert.eq(t.find().hint(goodspec).toArray().length, 10, "basic insert didn't work");
assert.eq(t.find({a: 3}).hint({_id: 1}).toArray()[0]._id,
          t.find({a: 3}).hint(goodspec).toArray()[0]._id,
          "hashindex lookup didn't work");

// make sure things with the same hash are not both returned
t.insert({a: 3.1});
assert.eq(t.find().count(), 11, "additional insert didn't work");
assert.eq(t.find({a: 3.1}).hint(goodspec).toArray().length, 1);
assert.eq(t.find({a: 3}).hint(goodspec).toArray().length, 1);
// test right obj is found
assert.eq(t.find({a: 3.1}).hint(goodspec).toArray()[0].a, 3.1);

// Make sure we're using the hashed index.
var explain = t.find({a: 1}).explain();
assert(isIxscan(explain.queryPlanner.winningPlan), "not using hashed index");

// SERVER-12222
// printjson( t.find({a : {$gte : 3 , $lte : 3}}).explain() )
// assert.eq( t.find({a : {$gte : 3 , $lte : 3}}).explain().cursor ,
//		cursorname ,
//		"not using hashed cursor");
var explain = t.find({c: 1}).explain();
assert(!isIxscan(explain.queryPlanner.winningPlan), "using irrelevant hashed index");

// Hash index used with a $in set membership predicate.
var explain = t.find({a: {$in: [1, 2]}}).explain();
printjson(explain);
assert(isIxscan(explain.queryPlanner.winningPlan), "not using hashed index");

// Hash index used with a singleton $and predicate conjunction.
var explain = t.find({$and: [{a: 1}]}).explain();
assert(isIxscan(explain.queryPlanner.winningPlan), "not using hashed index");

// Hash index used with a non singleton $and predicate conjunction.
var explain = t.find({$and: [{a: {$in: [1, 2]}}, {a: {$gt: 1}}]}).explain();
assert(isIxscan(explain.queryPlanner.winningPlan), "not using hashed index");

// test creation of index based on hash of _id index
var goodspec2 = {'_id': "hashed"};
t.ensureIndex(goodspec2);
assert.eq(t.getIndexes().length, 3, "_id index didn't get created");

var newid = t.findOne()["_id"];
assert.eq(t.find({_id: newid}).hint({_id: 1}).toArray()[0]._id,
          t.find({_id: newid}).hint(goodspec2).toArray()[0]._id,
          "using hashed index and different index returns different docs");

// test creation of sparse hashed index
var sparseindex = {b: "hashed"};
t.ensureIndex(sparseindex, {"sparse": true});
assert.eq(t.getIndexes().length, 4, "sparse index didn't get created");

// test sparse index has smaller total items on after inserts
for (i = 0; i < 10; i++) {
    t.insert({b: i});
}
var totalb = t.find().hint(sparseindex).toArray().length;
assert.eq(totalb, 10, "sparse index has wrong total");

var total = t.find().hint({"_id": 1}).toArray().length;
var totala = t.find().hint(goodspec).toArray().length;
assert.eq(total, totala, "non-sparse index has wrong total");
assert.lt(totalb, totala, "sparse index should have smaller total");
