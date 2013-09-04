a = db.getSisterDB( "copydb2-test-a" );
b = db.getSisterDB( "copydb2-test-b" );

a.dropDatabase();
b.dropDatabase();
a.removeAllUsers();
b.removeAllUsers();

a.foo.save( { a : 1 } );

a.addUser( "chevy" , "chase", jsTest.basicUserRoles, 1 );

assert.eq( 1 , a.foo.count() , "A" );
assert.eq( 0 , b.foo.count() , "B" );

// SERVER-727
a.copyDatabase( a._name , b._name, "" , "chevy" , "chase" );
assert.eq( 1 , a.foo.count() , "C" );
assert.eq( 1 , b.foo.count() , "D" );
