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
   return RegExp( "^" + RegExp.escape(coll + "") + "-.*" )
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

sh.getBalancerHost = function() {   
    var x = db.getSisterDB("config").locks.findOne({ _id: "balancer" });
    if( x == null ){
        print("config.locks collection does not contain balancer lock. be sure you are connected to a mongos");
        return ""
    }
    return x.process.match(/[^:]+:[^:]+/)[0]
}

sh.stopBalancer = function( timeout, interval ) {
    sh.setBalancerState( false )
    sh.waitForBalancer( false, timeout, interval )
}

sh.startBalancer = function( timeout, interval ) {
    sh.setBalancerState( true )
    sh.waitForBalancer( true, timeout, interval )
}

sh.waitForDLock = function( lockId, onOrNot, timeout, interval ){
    
    // Wait for balancer to be on or off
    // Can also wait for particular balancer state
    var state = onOrNot
    
    var beginTS = undefined
    if( state == undefined ){
        var currLock = db.getSisterDB( "config" ).locks.findOne({ _id : lockId })
        if( currLock != null ) beginTS = currLock.ts
    }
        
    var lockStateOk = function(){
        var lock = db.getSisterDB( "config" ).locks.findOne({ _id : lockId })

        if( state == false ) return ! lock || lock.state == 0
        if( state == true ) return lock && lock.state == 2
        if( state == undefined ) return (beginTS == undefined && lock) || 
                                        (beginTS != undefined && ( !lock || lock.ts + "" != beginTS + "" ) )
        else return lock && lock.state == state
    }
    
    assert.soon( lockStateOk,
                 "Waited too long for lock " + lockId + " to " + 
                      (state == true ? "lock" : ( state == false ? "unlock" : 
                                       "change to state " + state ) ),
                 timeout,
                 interval
    )
}

sh.waitForPingChange = function( activePings, timeout, interval ){
    
    var isPingChanged = function( activePing ){
        var newPing = db.getSisterDB( "config" ).mongos.findOne({ _id : activePing._id })
        return ! newPing || newPing.ping + "" != activePing.ping + ""
    }
    
    // First wait for all active pings to change, so we're sure a settings reload
    // happened
    
    // Timeout all pings on the same clock
    var start = new Date()
    
    var remainingPings = []
    for( var i = 0; i < activePings.length; i++ ){
        
        var activePing = activePings[ i ]
        print( "Waiting for active host " + activePing._id + " to recognize new settings... (ping : " + activePing.ping + ")" )
       
        // Do a manual timeout here, avoid scary assert.soon errors
        var timeout = timeout || 30000;
        var interval = interval || 200;
        while( isPingChanged( activePing ) != true ){
            if( ( new Date() ).getTime() - start.getTime() > timeout ){
                print( "Waited for active ping to change for host " + activePing._id + 
                       ", a migration may be in progress or the host may be down." )
                remainingPings.push( activePing )
                break
            }
            sleep( interval )   
        }
    
    }
    
    return remainingPings
}

sh.waitForBalancerOff = function( timeout, interval ){
    
    var pings = db.getSisterDB( "config" ).mongos.find().toArray()
    var activePings = []
    for( var i = 0; i < pings.length; i++ ){
        if( ! pings[i].waiting ) activePings.push( pings[i] )
    }
    
    print( "Waiting for active hosts..." )
    
    activePings = sh.waitForPingChange( activePings, 60 * 1000 )
    
    // After 1min, we assume that all hosts with unchanged pings are either 
    // offline (this is enough time for a full errored balance round, if a network
    // issue, which would reload settings) or balancing, which we wait for next
    // Legacy hosts we always have to wait for
    
    print( "Waiting for the balancer lock..." )
    
    // Wait for the balancer lock to become inactive
    // We can guess this is stale after 15 mins, but need to double-check manually
    try{ 
        sh.waitForDLock( "balancer", false, 15 * 60 * 1000 )
    }
    catch( e ){
        print( "Balancer still may be active, you must manually verify this is not the case using the config.changelog collection." )
        throw e
    }
        
    print( "Waiting again for active hosts after balancer is off..." )
    
    // Wait a short time afterwards, to catch the host which was balancing earlier
    activePings = sh.waitForPingChange( activePings, 5 * 1000 )
    
    // Warn about all the stale host pings remaining
    for( var i = 0; i < activePings.length; i++ ){
        print( "Warning : host " + activePings[i]._id + " seems to have been offline since " + activePings[i].ping )
    }
    
}

sh.waitForBalancer = function( onOrNot, timeout, interval ){
    
    // If we're waiting for the balancer to turn on or switch state or
    // go to a particular state
    if( onOrNot ){
        // Just wait for the balancer lock to change, can't ensure we'll ever see it
        // actually locked
        sh.waitForDLock( "balancer", undefined, timeout, interval )
    }
    else {
        // Otherwise we need to wait until we're sure balancing stops
        sh.waitForBalancerOff( timeout, interval )
    }
    
}

sh.disableBalancing = function( coll ){
    var dbase = db
    if( coll instanceof DBCollection ) dbase = coll.getDB()
    dbase.getSisterDB( "config" ).collections.update({ _id : coll + "" }, { $set : { "noBalance" : true } })
}

sh.enableBalancing = function( coll ){
    var dbase = db
    if( coll instanceof DBCollection ) dbase = coll.getDB()
    dbase.getSisterDB( "config" ).collections.update({ _id : coll + "" }, { $set : { "noBalance" : false } })
}

/*
 * Can call _lastMigration( coll ), _lastMigration( db ), _lastMigration( st ), _lastMigration( mongos ) 
 */
sh._lastMigration = function( ns ){
    
    var coll = null
    var dbase = null
    var config = null
    
    if( ! ns ){
        config = db.getSisterDB( "config" )
    }   
    else if( ns instanceof DBCollection ){
        coll = ns
        config = coll.getDB().getSisterDB( "config" )
    }
    else if( ns instanceof DB ){
        dbase = ns
        config = dbase.getSisterDB( "config" )
    }
    else if( ns instanceof ShardingTest ){
        config = ns.s.getDB( "config" )
    }
    else if( ns instanceof Mongo ){
        config = ns.getDB( "config" )
    }
    else {
        // String namespace
        ns = ns + ""
        if( ns.indexOf( "." ) > 0 ){
            config = db.getSisterDB( "config" )
            coll = db.getMongo().getCollection( ns )
        }
        else{
            config = db.getSisterDB( "config" )
            dbase = db.getSisterDB( ns )
        }
    }
        
    var searchDoc = { what : /^moveChunk/ }
    if( coll ) searchDoc.ns = coll + ""
    if( dbase ) searchDoc.ns = new RegExp( "^" + dbase + "\\." )
        
    var cursor = config.changelog.find( searchDoc ).sort({ time : -1 }).limit( 1 )
    if( cursor.hasNext() ) return cursor.next()
    else return null
}

