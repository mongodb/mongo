
t = db.dbadmin;
t.save( { x : 1 } );

before = db._adminCommand( "serverStatus" )
db._adminCommand( "closeAllDatabases" );
after = db._adminCommand( "serverStatus" );
assert( before.mem.mapped > after.mem.mapped , "closeAllDatabases does something before:" + tojson( before ) + " after:" + tojson( after ) );

t.save( { x : 1 } );

res = db._adminCommand( "listDatabases" );
assert( res.databases.length > 0 , "listDatabases 1" );
// TODO: add more tests here
