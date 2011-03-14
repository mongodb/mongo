// Test for SERVER-2746

coll = db.geo10
coll.drop();

db.geo10.ensureIndex({ c: '2d', t: 1 }, { min: 0, max: Math.pow(2, 31) })
print( db.getLastError() )

print( Math.pow(2, 30 ))

db.geo10.insert({ c : [1,1], t : 1})
print( db.getLastError() )

db.geo10.insert({ c : [3600, 3600], t : 1 })
print( db.getLastError() )

db.geo10.insert({ c : [0.001, 0.001], t : 1 })
print( db.getLastError() )

printjson( db.system.indexes.find().toArray() )


