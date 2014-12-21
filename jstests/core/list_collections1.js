// Test the listCollections command

mydb = db.getSisterDB( "list_collections1" );
mydb.dropDatabase();

mydb.foo.insert( { x : 5 } );

mydb.runCommand( { create : "bar", temp : true } );

res = mydb.runCommand( "listCollections", { cursor : {} } );
collections = new DBCommandCursor( db.getMongo(), res ).toArray();

bar = collections.filter( function(x){ return x.name == "bar"; } )[0];
foo = collections.filter( function(x){ return x.name == "foo" ; } )[0];

assert( bar );
assert( foo );

assert.eq( bar.name, mydb.bar.getName() );
assert.eq( foo.name, mydb.foo.getName() );

assert( mydb.bar.temp, tojson( bar ) );

mydb.dropDatabase();
