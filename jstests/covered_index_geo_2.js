var coll = db.getCollection("covered_geo_2")
coll.drop()

coll.insert({_id : 1, loc1 : [ 5 , 5 ], type1 : "type1",
    loc2 : [ 5 , 5 ], type2 : 1})
coll.insert({_id : 2, loc1 : [ 6 , 6 ], type1 : "type2",
    loc2 : [ 5 , 5 ], type2 : 2})
coll.insert({_id : 3, loc1 : [ 7 , 7 ], type1 : "type3",
    loc2 : [ 5 , 5 ], type2 : 3})

coll.ensureIndex({loc1 : "2dsphere", type1 : 1});
coll.ensureIndex({type2: 1, loc2 : "2dsphere"});

var plan = coll.find({loc1 : [ 6 , 6 ]}, {loc1:1, type1:1, _id:0}).hint({loc1:"2dsphere", type1:1}).explain();
assert.eq(false, plan.indexOnly, "geo.2.1 - indexOnly should be false on a non covered query")
assert.neq(0, plan.nscannedObjects, "geo.2.1 - nscannedObjects should not be 0 for a non covered query")

var plan = coll.find({loc1 : [ 6 , 6 ]}, {type1:1, _id:0}).hint({loc1:"2dsphere", type1:1}).explain();
assert.eq(false, plan.indexOnly, "geo.2.2 - indexOnly should be false for a non covered query")
assert.neq(0, plan.nscannedObjects, "geo.2.2 - nscannedObjects should not be 0 for a non covered query")

// The test below should not be covered but is due to https://jira.mongodb.org/browse/SERVER-8621
var plan = coll.find({type2 : {$lt:3}}, {type2:1, loc2:1, _id:0}).hint({type2: 1, loc2:"2dsphere"}).explain();
assert.eq(true, plan.indexOnly, "geo.2.3 - indexOnly should be false for a non covered query")
assert.eq(0, plan.nscannedObjects, "geo.2.3 - nscannedObjects should notbe 0 for a non covered query")

var plan = coll.find({type2 : {$lt:3}}, {type2:1, _id:0}).hint({type2:1, loc2:"2dsphere"}).explain();
assert.eq(true, plan.indexOnly, "geo.2.4 - indexOnly should be true for a covered query")
assert.eq(0, plan.nscannedObjects, "geo.2.4 - nscannedObjects should be 0 for a covered query")

print("all tests passed")