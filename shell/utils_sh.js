sh = function() { return "try sh.help();" }


sh._checkMongos = function() {
    var x = db.runCommand( "ismaster" );
    if ( x.msg != "isdbgrid" )
        throw "not connected to a mongos"
}

sh._adminCommand = function( cmd ) {
    sh._checkMongos();
    var res = db.getSisterDB( "admin" ).runCommand( cmd );

    if ( res == null || ! res.ok ) {
        print( "command failed: " + tojson( res ) )
    }

    return res;
}

sh.help = function() {
    print( "\tsh.enableSharding(dbname)                 enables sharding on the database dbname" )
    print( "\tsh.status()                               prints a general overview of the cluster" )
}

sh.status = function( verbose , configDB ) { 
    // TODO: move the actual commadn here
    printShardingStatus( configDB , verbose );
}

sh.enableSharding = function( dbname ) { 
    assert( dbname , "need a valid dbname" )
    sh._adminCommand( { enableSharding : dbname } )
}



