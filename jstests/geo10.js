// Test for SERVER-2746

coll = db.geo10
coll.drop();

db.geo10.ensureIndex( { c : '2d', t : 1 }, { min : 0, max : Math.pow( 2, 31 ) } )
assert( db.getLastError(), "A0" )
assert( db.system.indexes.count() == 1, "A1" )

db.geo10.ensureIndex( { c : '2d', t : 1 }, { min : 0, max : Math.pow( 2, 30 ) } )
assert( db.getLastError() == null, "B" )
assert( db.system.indexes.count() == 2, "A3" )

printjson( db.system.indexes.find().toArray() )

db.geo10.insert( { c : [ 1, 1 ], t : 1 } )
assert( db.getLastError() == null, "C" )

db.geo10.insert( { c : [ 3600, 3600 ], t : 1 } )
assert( db.getLastError() == null, "D" )

db.geo10.insert( { c : [ 0.001, 0.001 ], t : 1 } )
assert( db.getLastError() == null, "E" )

