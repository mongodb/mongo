
t = db["users"];
t.remove( {} );

t.save({fn: "john", ln: "doe"})
t.save({fn: "jack", ln: "doe"})
t.save({fn: "john", ln: "smith"})
t.save({fn: "jack", ln: "black"})
t.save({fn: "bob", ln: "murray"})
assert.eq( t.findOne({ln: "doe"}).fn, "john", "Cannot find right record" );
assert.eq( t.find({}).length, 5, "Not right length" );

// use simple index
t.ensureIndex({ln: 1});
assert.eq( t.find({ln: "doe"}).explain().indexOnly, false, "Find using covered index but all fields are returned");
assert.eq( t.find({ln: "doe"}, {ln: 1}).explain().indexOnly, false, "Find using covered index but _id is returned");
assert.eq( t.find({ln: "doe"}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");

// use compound index
t.dropIndex({ln: 1})
t.ensureIndex({ln: 1, fn: 1});
// return 1 field
assert.eq( t.find({ln: "doe"}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// return both fields, multiple docs returned
assert.eq( t.find({ln: "doe"}, {ln: 1, fn: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// match 1 record using both fields
assert.eq( t.find({ln: "doe", fn: "john"}, {ln: 1, fn: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// change ordering
assert.eq( t.find({fn: "john", ln: "doe"}, {fn: 1, ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// ask from 2nd index key
assert.eq( t.find({fn: "john"}, {fn: 1, _id: 0}).explain().indexOnly, false, "Find is using covered index, but doesnt have 1st key");

// repeat above but with _id field
t.dropIndex({ln: 1})
t.ensureIndex({ln: 1, fn: 1});
// return 1 field
assert.eq( t.find({ln: "doe"}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// return both fields, multiple docs returned
assert.eq( t.find({ln: "doe"}, {ln: 1, fn: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// match 1 record using both fields
assert.eq( t.find({ln: "doe", fn: "john"}, {ln: 1, fn: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// change ordering
assert.eq( t.find({fn: "john", ln: "doe"}, {fn: 1, ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
// ask from 2nd index key
assert.eq( t.find({fn: "john"}, {fn: 1, _id: 0}).explain().indexOnly, false, "Find is using covered index, but doesnt have 1st key");


assert(t.validate().valid);

