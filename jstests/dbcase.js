
a = db.getSisterDB( "dbcasetest_dbnamea" )
b = db.getSisterDB( "dbcasetest_dbnameA" )

a.dropDatabase();
b.dropDatabase();

a.foo.save( { x : 1 } )
z = db.getLastErrorObj();
assert.eq( 0 , z.code || 0 , "A : " + tojson(z) )

b.foo.save( { x : 1 } )
z = db.getLastErrorObj();
assert.eq( 13297 , z.code || 0 , "B : " + tojson(z) )

print( db.getMongo().getDBNames() )

a.dropDatabase();
b.dropDatabase();

print( db.getMongo().getDBNames() )


