// Simple covered index query test with unique index

var coll = db.getCollection("covered_simple_2")
coll.drop()
for (i=0;i<10;i++) {
    coll.insert({foo:i})
}
coll.insert({foo:"string"})
coll.insert({foo:{bar:1}})
coll.insert({foo:null})
coll.ensureIndex({foo:1},{unique:true})

// Test equality with int value
var plan = coll.find({foo:1}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.1 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.1 - nscannedObjects should be 0 for covered query")

// Test equality with string value
var plan = coll.find({foo:"string"}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.2 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.2 - nscannedObjects should be 0 for covered query")

// Test equality with int value on a dotted field
var plan = coll.find({foo:{bar:1}}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.3 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.3 - nscannedObjects should be 0 for covered query")

// Test no query
var plan = coll.find({}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.4 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.4 - nscannedObjects should be 0 for covered query")

// Test range query
var plan = coll.find({foo:{$gt:2,$lt:6}}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.5 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.5 - nscannedObjects should be 0 for covered query")

// Test in query
var plan = coll.find({foo:{$in:[5,8]}}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.6 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "simple.2.6 - nscannedObjects should be 0 for covered query")

// Test not in query
var plan = coll.find({foo:{$nin:[5,8]}}, {foo:1, _id:0}).hint({foo:1}).explain()
assert.eq(true, plan.indexOnly, "simple.2.7 - indexOnly should be true on covered query")
// this should be 0 but is not due to bug https://jira.mongodb.org/browse/SERVER-3187
assert.eq(13, plan.nscannedObjects, "simple.2.7 - nscannedObjects should be 0 for covered query")

print ('all tests pass')