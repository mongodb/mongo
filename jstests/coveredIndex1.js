
t = db["jstests_coveredIndex1"];
t.drop();

t.save({fn: "john", ln: "doe"})
t.save({fn: "jack", ln: "doe"})
t.save({fn: "john", ln: "smith"})
t.save({fn: "jack", ln: "black"})
t.save({fn: "bob", ln: "murray"})
t.save({fn: "aaa", ln: "bbb", obj: {a: 1, b: "blah"}})
assert.eq( t.findOne({ln: "doe"}).fn, "john", "Cannot find right record" );
assert.eq( t.count(), 6, "Not right length" );

// use simple index
t.ensureIndex({ln: 1});
assert.eq( t.find({ln: "doe"}).explain().indexOnly, false, "Find using covered index but all fields are returned");
assert.eq( t.find({ln: "doe"}, {ln: 1}).explain().indexOnly, false, "Find using covered index but _id is returned");
assert.eq( t.find({ln: "doe"}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");

// this time, without a query spec
// SERVER-2109
//assert.eq( t.find({}, {ln: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");
assert.eq( t.find({}, {ln: 1, _id: 0}).hint({ln: 1}).explain().indexOnly, true, "Find is not using covered index");

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
t.dropIndex({ln: 1, fn: 1})
t.ensureIndex({_id: 1, ln: 1});
// return 1 field
assert.eq( t.find({_id: 123}, {_id: 1}).explain().indexOnly, true, "Find is not using covered index");
// match 1 record using both fields
assert.eq( t.find({_id: 123, ln: "doe"}, {ln: 1}).explain().indexOnly, true, "Find is not using covered index");
// change ordering
assert.eq( t.find({ln: "doe", _id: 123}, {ln: 1, _id: 1}).explain().indexOnly, true, "Find is not using covered index");
// ask from 2nd index key
assert.eq( t.find({ln: "doe"}, {ln: 1}).explain().indexOnly, false, "Find is using covered index, but doesnt have 1st key");

// repeat above but with embedded obj
t.dropIndex({_id: 1, ln: 1})
t.ensureIndex({obj: 1});
assert.eq( t.find({"obj.a": 1}, {obj: 1}).explain().indexOnly, false, "Shouldnt use index when introspecting object");
assert.eq( t.find({obj: {a: 1, b: "blah"}}).explain().indexOnly, false, "Index doesnt have all fields to cover");
assert.eq( t.find({obj: {a: 1, b: "blah"}}, {obj: 1, _id: 0}).explain().indexOnly, true, "Find is not using covered index");

// repeat above but with index on sub obj field
t.dropIndex({obj: 1});
t.ensureIndex({"obj.a": 1, "obj.b": 1})
assert.eq( t.find({"obj.a": 1}, {obj: 1}).explain().indexOnly, false, "Shouldnt use index when introspecting object");

assert(t.validate().valid);

