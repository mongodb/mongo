sh = function() { return "try sh.help();" }

sh._checkMongos = function() {
    var x = db.runCommand( "ismaster" );
    if ( x.msg != "isdbgrid" )
        throw "not connected to a mongos"
}

sh._checkFullName = function( fullName ) {
    assert( fullName , "neeed a full name" )
    assert( fullName.indexOf( "." ) > 0 , "name needs to be fully qualified <db>.<collection>'" )
}

sh._adminCommand = function( cmd , skipCheck ) {
    if ( ! skipCheck ) sh._checkMongos();
    var res = db.getSisterDB( "admin" ).runCommand( cmd );

    if ( res == null || ! res.ok ) {
        print( "command failed: " + tojson( res ) )
    }

    return res;
}

sh._dataFormat = function( bytes ){
   if( bytes < 1024 ) return Math.floor( bytes ) + "b"
   if( bytes < 1024 * 1024 ) return Math.floor( bytes / 1024 ) + "kb"
   if( bytes < 1024 * 1024 * 1024 ) return Math.floor( ( Math.floor( bytes / 1024 ) / 1024 ) * 100 ) / 100 + "Mb"
   return Math.floor( ( Math.floor( bytes / ( 1024 * 1024 ) ) / 1024 ) * 100 ) / 100 + "Gb"
}

sh._collRE = function( coll ){
   return RegExp( "^" + (coll + "").replace(/\./g, "\\.") + "-.*" )
}

sh._pchunk = function( chunk ){
   return "[" + tojson( chunk.min ) + " -> " + tojson( chunk.max ) + "]"
}

sh.help = function() {
    print( "\tsh.addShard( host )                       server:port OR setname/server:port" )
    print( "\tsh.enableSharding(dbname)                 enables sharding on the database dbname" )
    print( "\tsh.shardCollection(fullName,key,unique)   shards the collection" );

    print( "\tsh.splitFind(fullName,find)               splits the chunk that find is in at the median" );
    print( "\tsh.splitAt(fullName,middle)               splits the chunk that middle is in at middle" );
    print( "\tsh.moveChunk(fullName,find,to)            move the chunk where 'find' is to 'to' (name of shard)");
    
    print( "\tsh.setBalancerState( <bool on or not> )   turns the balancer on or off true=on, false=off" );
    print( "\tsh.getBalancerState()                     return true if on, off if not" );
    print( "\tsh.isBalancerRunning()                    return true if the balancer is running on any mongos" );
    
    print( "\tsh.status()                               prints a general overview of the cluster" )
}

sh.status = function( verbose , configDB ) { 
    // TODO: move the actual commadn here
    printShardingStatus( configDB , verbose );
}

sh.addShard = function( url ){
    sh._adminCommand( { addShard : url } , true )
}

sh.enableSharding = function( dbname ) { 
    assert( dbname , "need a valid dbname" )
    sh._adminCommand( { enableSharding : dbname } )
}

sh.shardCollection = function( fullName , key , unique ) {
    sh._checkFullName( fullName )
    assert( key , "need a key" )
    assert( typeof( key ) == "object" , "key needs to be an object" )
    
    var cmd = { shardCollection : fullName , key : key }
    if ( unique ) 
        cmd.unique = true;

    sh._adminCommand( cmd )
}

sh.splitFind = function( fullName , find ) {
    sh._checkFullName( fullName )
    sh._adminCommand( { split : fullName , find : find } )
}

sh.splitAt = function( fullName , middle ) {
    sh._checkFullName( fullName )
    sh._adminCommand( { split : fullName , middle : middle } )
}

sh.moveChunk = function( fullName , find , to ) {
    sh._checkFullName( fullName );
    return sh._adminCommand( { moveChunk : fullName , find : find , to : to } )
}

sh.setBalancerState = function( onOrNot ) { 
    db.getSisterDB( "config" ).settings.update({ _id: "balancer" }, { $set : { stopped: onOrNot ? false : true } }, true );
}

sh.getBalancerState = function() {
    var x = db.getSisterDB( "config" ).settings.findOne({ _id: "balancer" } )
    if ( x == null )
        return true;
    return ! x.stopped;
}

sh.isBalancerRunning = function () {
    var x = db.getSisterDB("config").locks.findOne({ _id: "balancer" });
    if (x == null) {
        print("config.locks collection empty or missing. be sure you are connected to a mongos");
        return false;
    }
    return x.state > 0;
}

sh.stopBalancer = function( timeout, interval ) {
    sh.setBalancerState( false )
    sh.waitForBalancer( false, timeout, interval )
}

sh.startBalancer = function( timeout, interval ) {
    sh.setBalancerState( true )
    sh.waitForBalancer( true, timeout, interval )
}

sh.waitForBalancer = function( onOrNot, timeout, interval ){
    
    if( onOrNot != undefined ){
        
        // Wait for balancer to be on or off
        // Can also wait for particular balancer state
        var state = null
        if( ! onOrNot ) state = 0
        else if( onOrNot == true ) state = 2
        else state = onOrNot
        
        assert.soon( function(){ var lock = db.getSisterDB( "config" ).locks.findOne( { _id : "balancer" } );
                                 return ( lock == null && state == 0 ) || ( lock != null && lock.state == state ) 
                     },
                     "waited too long for balancer to " + ( state > 0 ? "start" : "stop" ) + " [ state : " + state + "]",
                     timeout,
                     interval
        )
        
    }
    else{
        
        // Wait for balancer to run at least once
        
        var lock = db.getSisterDB( "config" ).locks.findOne({ _id : "balancer" })
        var ts = lock ? lock.ts : ""
        
        assert.soon( function(){ var lock = db.getSisterDB( "config" ).locks.findOne({ _id : "balancer" });
                                 if( ! lock ) return false;
                                 return lock.ts != ts
                                },
                                "waited too long for balancer to activate",
                                timeout,
                                interval
        )        
    }
}

