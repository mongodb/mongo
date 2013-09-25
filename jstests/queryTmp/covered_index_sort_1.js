// Simple covered index query test with sort

var coll = db.getCollection("covered_sort_1")
coll.drop()
for (i=0;i<10;i++) {
    coll.insert({foo:i})
}
for (i=0;i<10;i++) {
    coll.insert({foo:i})
}
for (i=0;i<5;i++) {
    coll.insert({bar:i})
}
coll.insert({foo:"1"})
coll.insert({foo:{bar:1}})
coll.insert({foo:null})
coll.ensureIndex({foo:1})

// Test no query and sort ascending
// NEW QUERY EXPLAIN
assert.eq(coll.find({}, {foo:1, _id:0}).sort({foo:1}).hint({foo:1}).itcount(), 28); 
/* NEW QUERY EXPLAIN
assert.eq(true, plan.indexOnly, "sort.1.1 - indexOnly should be true on covered query")
*/
/* NEW QUERY EXPLAIN
assert.eq(0, plan.nscannedObjects, "sort.1.1 - nscannedObjects should be 0 for covered query")
*/

// Test no query and sort descending
// NEW QUERY EXPLAIN
assert.eq(coll.find({}, {foo:1, _id:0}).sort({foo:-1}).hint({foo:1}).itcount(), 28);
/* NEW QUERY EXPLAIN
assert.eq(true, plan.indexOnly, "sort.1.2 - indexOnly should be true on covered query")
*/
/* NEW QUERY EXPLAIN
assert.eq(0, plan.nscannedObjects, "sort.1.2 - nscannedObjects should be 0 for covered query")
*/

// Test range query with sort
// NEW QUERY EXPLAIN
assert.eq(coll.find({foo:{$gt:2}}, {foo:1, _id:0}).sort({foo:-1}).hint({foo:1}).itcount(), 14);
/* NEW QUERY EXPLAIN
assert.eq(true, plan.indexOnly, "sort.1.5 - indexOnly should be true on covered query")
*/
/* NEW QUERY EXPLAIN
assert.eq(0, plan.nscannedObjects, "sort.1.5 - nscannedObjects should be 0 for covered query")
*/

print ('all tests pass')
