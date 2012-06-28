// mostly for testing mongos w/replica sets


s = new ShardingTest( "rs2" , 2 , 1 , 1 , { rs : true , chunksize : 1 } )

db = s.getDB( "test" )
t = db.foo

// -------------------------------------------------------------------------------------------
// ---------- test that config server updates when replica set config changes ----------------
// -------------------------------------------------------------------------------------------


db.foo.save( { _id : 5 ,x : 17 } )
assert.eq( 1 , db.foo.count() );

s.config.databases.find().forEach( printjson )
s.config.shards.find().forEach( printjson )

serverName = s.getServerName( "test" ) 

function countNodes(){
    var x = s.config.shards.findOne( { _id : serverName } );
    return x.host.split( "," ).length
}

assert.eq( 3 , countNodes() , "A1" )

rs = s.getRSEntry( serverName );
rs.test.add()
try {
    rs.test.reInitiate();
}
catch ( e ){
    // this os ok as rs's may close connections on a change of master
    print( e );
}

assert.soon( 
    function(){
        try {
            printjson( rs.test.getMaster().getDB("admin").runCommand( "isMaster" ) )
            s.config.shards.find().forEach( printjsononeline );
            return countNodes() == 4;
        }
        catch ( e ){
            print( e );
        }
    } , "waiting for config server to update" , 180 * 1000 , 1000 );

// cleanup after adding node
for ( i=0; i<5; i++ ){
    try {
        db.foo.findOne();
    }
    catch ( e ){}
}

// -------------------------------------------------------------------------------------------
// ---------- test routing to slaves ----------------
// -------------------------------------------------------------------------------------------

// --- not sharded ----

m = new Mongo( s.s.name );
ts = m.getDB( "test" ).foo

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

for ( i=0; i<10; i++ )
    assert.eq( 17 , ts.findOne().x , "B1" )

m.setSlaveOk()

// Confusingly, v2.0 mongos does not actually update the secondary status of any members until after the first 
// ReplicaSetMonitorWatcher round.  Wait for that here.
ReplSetTest.awaitRSClientHosts( m, rs.test.getSecondaries()[0], { secondary : true } )

for ( i=0; i<10; i++ )
    assert.eq( 17 , ts.findOne().x , "B2" )

after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

printjson( before )
printjson( after )

assert.lte( before.query + 10 , after.query , "B3" )

// --- add more data ----

db.foo.ensureIndex( { x : 1 } )

for ( i=0; i<100; i++ ){
    if ( i == 17 ) continue;
    db.foo.insert( { x : i } )
}
db.getLastError( 3 , 10000 );

assert.eq( 100 , ts.count() , "B4" )
assert.eq( 100 , ts.find().itcount() , "B5" )
assert.eq( 100 , ts.find().batchSize(5).itcount() , "B6" )

t.find().batchSize(3).next();
gc(); gc(); gc();

// --- sharded ----

assert.eq( 100 , db.foo.count() , "C1" )

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { x : 1 } } );

// Changes in this test were backported from 2.1.x into 2.0.x, but the supporting infrastructure did
// not exists in 2.0, so ... create the functions we need for this test
//
if ( ! sh.waitForPingChange ) {
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
}

if ( ! sh.waitForBalancerOff ) {
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
}

if ( ! sh.waitForDLock ) {
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
}

if ( ! sh.waitForBalancer ) {
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
}

if ( ! s.stopBalancer ) {
    s.stopBalancer = function( timeout, interval ) {
        this.setBalancer( false )
    
        if( typeof db == "undefined" ) db = undefined
        var oldDB = db
    
        db = this.config
        sh.waitForBalancer( false, timeout, interval )
        db = oldDB
    }
}

// We're doing some manual chunk stuff, so stop the balancer first
s.stopBalancer()

assert.eq( 100 , t.count() , "C2" )
s.adminCommand( { split : "test.foo" , middle : { x : 50 } } )

db.printShardingStatus()

other = s.config.shards.findOne( { _id : { $ne : serverName } } );
s.adminCommand( { moveChunk : "test.foo" , find : { x : 10 } , to : other._id } )
assert.eq( 100 , t.count() , "C3" )

assert.eq( 50 , rs.test.getMaster().getDB( "test" ).foo.count() , "C4" )

// Let the balancer start again
s.setBalancer( true )

// by non-shard key

m = new Mongo( s.s.name );
ts = m.getDB( "test" ).foo

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

for ( i=0; i<10; i++ )
    assert.eq( 17 , ts.findOne( { _id : 5 } ).x , "D1" )

m.setSlaveOk()
for ( i=0; i<10; i++ )
    assert.eq( 17 , ts.findOne( { _id : 5 } ).x , "D2" )

after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

assert.lte( before.query + 10 , after.query , "D3" )

// by shard key

m = new Mongo( s.s.name );
ts = m.getDB( "test" ).foo

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

for ( i=0; i<10; i++ )
    assert.eq( 57 , ts.findOne( { x : 57 } ).x , "E1" )

m.setSlaveOk()
for ( i=0; i<10; i++ )
    assert.eq( 57 , ts.findOne( { x : 57 } ).x , "E2" )

after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

assert.lte( before.query + 10 , after.query , "E3" )

assert.eq( 100 , ts.count() , "E4" )
assert.eq( 100 , ts.find().itcount() , "E5" )
printjson( ts.find().batchSize(5).explain() )

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters
// Careful, mongos can poll the masters here too unrelated to the query, 
// resulting in this test failing sporadically if/when there's a delay here.
assert.eq( 100 , ts.find().batchSize(5).itcount() , "E6" )
after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters
assert.eq( before.query + before.getmore , after.query + after.getmore , "E6.1" )

assert.eq( 100 , ts.find().batchSize(5).itcount() , "F1" )

for ( i=0; i<10; i++ ) {
    m = new Mongo( s.s.name );
    m.setSlaveOk();
    ts = m.getDB( "test" ).foo
    assert.eq( 100 , ts.find().batchSize(5).itcount() , "F2." + i )
}

for ( i=0; i<10; i++ ) {
    m = new Mongo( s.s.name );
    ts = m.getDB( "test" ).foo
    assert.eq( 100 , ts.find().batchSize(5).itcount() , "F3." + i )
}


printjson( db.adminCommand( "getShardMap" ) );


s.stop()
