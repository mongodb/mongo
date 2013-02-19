var coll = db.getCollection("covered_geo_1")
coll.drop()

coll.insert({_id : 1, loc : [ 5 , 5 ], type : "type1"})
coll.insert({_id : 2, loc : [ 6 , 6 ], type : "type2"})
coll.insert({_id : 3, loc : [ 7 , 7 ], type : "type3"})

coll.ensureIndex({loc : "2d", type : 1});

var plan = coll.find({loc : [ 6 , 6 ]}, {loc:1, type:1, _id:0}).hint({loc:"2d", type:1}).explain();
assert.eq(false, plan.indexOnly, "geo.1.1 - indexOnly should be false on a non covered query")
assert.neq(0, plan.nscannedObjects, "geo.1.1 - nscannedObjects should not be 0 for a non covered query")

var plan = coll.find({loc : [ 6 , 6 ]}, {type:1, _id:0}).hint({loc:"2d", type:1}).explain();
assert.eq(false, plan.indexOnly, "geo.1.2 - indexOnly should be false on a non covered query")
assert.neq(0, plan.nscannedObjects, "geo.1.2 - nscannedObjects should not be 0 for a non covered query")

print("all tests passed")