
t = db.dbadmin;
t.save( { x : 1 } );

before = db._adminCommand( "serverStatus" )
if ( before.mem.supported ){
    cmdres = db._adminCommand( "closeAllDatabases" );
    after = db._adminCommand( "serverStatus" );
    assert( before.mem.mapped > after.mem.mapped , "closeAllDatabases does something before:" + tojson( before.mem ) + " after:" + tojson( after.mem ) + " cmd res:" + tojson( cmdres ) );
    print( before.mem.mapped + " -->> " + after.mem.mapped );
}
else {
    print( "can't test serverStatus on this machine" );
}

t.save( { x : 1 } );

res = db._adminCommand( "listDatabases" );
assert( res.databases && res.databases.length > 0 , "listDatabases 1 " + tojson(res) );

now = new Date();
x = db._adminCommand( "ismaster" );
assert( x.ismaster , "ismaster failed: " + tojson( x ) )
assert( x.localTime, "ismaster didn't include time: " + tojson(x))
localTimeSkew = x.localTime - now
if ( localTimeSkew >= 50 ) {
    print( "Warning: localTimeSkew " + localTimeSkew + " > 50ms." )
}
assert.lt( localTimeSkew, 500, "isMaster.localTime" )

before = db.runCommand( "serverStatus" )
printjson(before);
sleep( 5000 )
after = db.runCommand( "serverStatus" )
printjson(after);
assert.lt( 2 , after.uptimeEstimate , "up1" )
assert.gt( after.uptimeEstimate , before.uptimeEstimate , "up2" )


assert.eq( db.runCommand( "buildinfo" ).gitVersion,
           db.getSisterDB( "local" ).startup_log.find().sort( { $natural: -1 } ).limit(1).next().buildinfo.gitVersion );
