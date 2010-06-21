
a = db.getSisterDB( "test_dbnamea" )
b = db.getSisterDB( "test_dbnameA" )

a.dropDatabase();
b.dropDatabase();

a.foo.save( { x : 1 } )
assert.eq( 0 , db.getLastErrorObj().code || 0 , "A" )

b.foo.save( { x : 1 } )
assert.eq( 13297 , db.getLastErrorObj().code || 0 , "A" )

a.dropDatabase();
b.dropDatabase();
