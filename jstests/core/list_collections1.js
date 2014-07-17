// Test the listCollections command

mydb = db.getSisterDB( "list_collections1" );
mydb.dropDatabase();

mydb.foo.insert( { x : 5 } );

mydb.runCommand( { create : "bar", temp : true } );

res = mydb.runCommand( "listCollections" );

bar = res.collections.filter( function(x){ return x.name == "bar"; } )[0];
foo = res.collections.filter( function(x){ return x.name == "foo" ; } )[0];

assert( bar );
assert( foo );

assert.eq( bar.name, mydb.bar.getName() );
assert.eq( foo.name, mydb.foo.getName() );

assert( mydb.bar.temp, tojson( bar ) );

assert.eq( mydb._getCollectionNamesSystemNamespaces(),
           mydb._getCollectionNamesCommand() );

assert.eq( mydb.getCollectionNames(),
           mydb._getCollectionNamesCommand() );

mydb.dropDatabase();
