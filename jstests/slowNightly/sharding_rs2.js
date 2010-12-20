// mostly for testing mongos w/replica sets


s = new ShardingTest( "rs2" , 2 , 1 , 1 , { rs : true , chunksize : 1 } )

db = s.getDB( "test" )

// -------------------------------------------------------------------------------------------
// ---------- test that config server updates when replica set config changes ----------------
// -------------------------------------------------------------------------------------------


db.foo.save( { x : 17 } )
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


// -------------------------------------------------------------------------------------------
// ---------- test routing to slaves ----------------
// -------------------------------------------------------------------------------------------

// --- not sharded ----

m = new Mongo( s.s.name );
t = m.getDB( "test" ).foo

before = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

for ( i=0; i<10; i++ )
    assert.eq( 17 , t.findOne().x , "B1" )

m.setSlaveOk()
for ( i=0; i<10; i++ )
    assert.eq( 17 , t.findOne().x , "B2" )

after = rs.test.getMaster().adminCommand( "serverStatus" ).opcounters

printjson( before )
printjson( after )

assert.eq( before.query + 10 , after.query , "B3" )

// --- sharded ----

t.ensureIndex( { x : 1 } )

for ( i=0; i<100; i++ ){
    if ( i == 17 ) continue;
    t.insert( { x : i } )
}

assert.eq( 100 , t.count() , "C1" )

/*
s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { x : 1 } } );

assert.eq( 100 , t.count() , "C2" )
try {
    s.adminCommand( { split : "test.foo" , middle : { x : 50 } } )
}
catch ( e ){
    printjson( e );
}

db.printShardingStatus()
throw 1

other : s.config.shards.findOne( { _id : { $ne : serverName } } );
s.adminCommand( { moveChunk : "test.foo" , find : { x : 10 } , to : other } )
assert.eq( 100 , t.count() , "C3" )

assert.eq( 50 , rs.test.getMaster().getDB( "test" ).foo.count() , "C4" )
*/
s.stop()
