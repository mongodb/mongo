



a = db.getSisterDB( "copydb-test-a" );
b = db.getSisterDB( "copydb-test-b" );

a.dropDatabase();
b.dropDatabase();

a.foo.save( { a : 1 } );

assert.eq( 1 , a.foo.count() , "A" );
assert.eq( 0 , b.foo.count() , "B" );

a.copyDatabase( a._name , b._name );

assert.eq( 1 , a.foo.count() , "C" );
assert.eq( 1 , b.foo.count() , "D" );

