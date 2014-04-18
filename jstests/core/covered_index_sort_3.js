// Compound index covered query tests with sort

var coll = db.getCollection("covered_sort_3")
coll.drop()
for (i=0;i<100;i++) {
    coll.insert({a:i, b:"strvar_"+(i%13), c:NumberInt(i%10)})
}
coll.insert
coll.ensureIndex({a:1,b:-1,c:1})

// Test no query, sort on all fields in index order
var plan = coll.find({}, {b:1, c:1, _id:0}).sort({a:1,b:-1,c:1}).hint({a:1, b:-1, c:1}).explain()
assert.eq(true, plan.indexOnly, "compound.1.1 - indexOnly should be true on covered query")
assert.eq(0, plan.nscannedObjects, "compound.1.1 - nscannedObjects should be 0 for covered query")

print ('all tests pass')
