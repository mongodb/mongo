// Check db name duplication constraint SERVER-2111

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

assert.neq( -1, db.getMongo().getDBNames().indexOf( a.getName() ) );
assert.eq( -1, db.getMongo().getDBNames().indexOf( b.getName() ) );
printjson( db.getMongo().getDBs().databases );

a.dropDatabase();
b.dropDatabase();

ai = db.getMongo().getDBNames().indexOf( a.getName() );
bi = db.getMongo().getDBNames().indexOf( b.getName() );
// One of these dbs may exist if there is a slave active, but they must
// not both exist.
assert( ai == -1 || bi == -1 );
printjson( db.getMongo().getDBs().databases );
