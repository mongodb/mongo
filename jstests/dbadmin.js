
t = db.dbadmin;
t.save( { x : 1 } );

before = db._adminCommand( "serverStatus" )
if ( before.mem.supported ){
    db._adminCommand( "closeAllDatabases" );
    after = db._adminCommand( "serverStatus" );
    assert( before.mem.mapped > after.mem.mapped , "closeAllDatabases does something before:" + tojson( before ) + " after:" + tojson( after ) );
}
else {
    print( "can't test serverStatus on this machine" );
}

t.save( { x : 1 } );

res = db._adminCommand( "listDatabases" );
assert( res.databases.length > 0 , "listDatabases 1" );

print( "BEFORE: " + tojson( before ) );
print( "AFTER : " + tojson( after ) );
// TODO: add more tests here
