s = new ShardingTest( "features3" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db = s.getDB( "test" );

// ---------- load some data -----

s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
N = 10000;
s.adminCommand( { split : "test.foo" , middle : { _id : N/2 } } )
s.adminCommand( { moveChunk : "test.foo", find : { _id : 3 } ,to : s.getNonPrimaries( "test" )[0] } )

for ( i=0; i<N; i++ )
    db.foo.insert( { _id : i } )
db.getLastError();
x = db.foo.stats();
assert.eq( "test.foo" , x.ns , "basic1" )
assert( x.sharded , "basic2" )
assert.eq( N , x.count , "total count" )
assert.eq( N / 2 , x.shards.shard0000.count , "count on shard0000" )
assert.eq( N / 2 , x.shards.shard0001.count , "count on shard0001" )
assert( x.totalIndexSize > 0 )
assert( x.numExtents > 0 )

db.bar.insert( { x : 1 } )
x = db.bar.stats();
assert.eq( 1 , x.count , "XXX1" )
assert.eq( "test.bar" , x.ns , "XXX2" )
assert( ! x.sharded , "XXX3: " + tojson(x) )

// Fork shell and start pulling back data
start = new Date()

print( "about to fork shell: " + Date() )

// TODO:  Still potential problem when our sampling of current ops misses when $where is active - 
// solution is to increase sleep time
parallelCommand = "try { while(true){" +
                  " db.foo.find( function(){ x = ''; for ( i=0; i<10000; i++ ){ x+=i; } sleep( 1000 ); return true; } ).itcount() " +
                  "}} catch(e){ print('PShell execution ended:'); printjson( e ) }"

join = startParallelShell( parallelCommand )
print( "after forking shell: " + Date() )

// Get all current $where operations
function getMine( printInprog ){
    
    var inprog = db.currentOp().inprog;
    
    if ( printInprog )
        printjson( inprog )
    
    // Find all the where queries
    var mine = []
    for ( var x=0; x<inprog.length; x++ ){
        if ( inprog[x].query && inprog[x].query.$where ){
            mine.push( inprog[x] )
        }
    }
    
    return mine;
}

var state = 0; // 0 = not found, 1 = killed, 
var killTime = null;
var i = 0;

assert.soon( function(){
    
    // Get all the current operations
    mine = getMine( state == 0 && i > 20 );
    i++;
    
    // Wait for the queries to start
    if ( state == 0 && mine.length > 0 ){
        // Queries started
        state = 1;
        // Kill all $where
        mine.forEach( function(z){ printjson( db.getSisterDB( "admin" ).killOp( z.opid ) ); }  )
        killTime = new Date()
    }
    // Wait for killed queries to end
    else if ( state == 1 && mine.length == 0 ){
        // Queries ended
        state = 2;
        return true;
    }
    
}, "Couldn't kill the $where operations.", 2 * 60 * 1000 )

print( "after loop: " + Date() );
assert( killTime , "timed out waiting too kill last mine:" + tojson(mine) )

assert.eq( 2 , state , "failed killing" );

killTime = (new Date()).getTime() - killTime.getTime()
print( "killTime: " + killTime );

assert.gt( 10000 , killTime , "took too long to kill" )

join()

end = new Date()

print( "elapsed: " + ( end.getTime() - start.getTime() ) );


x = db.runCommand( "fsync" )
assert( ! x.ok , "fsync not on admin should fail : " + tojson( x ) );
assert( x.errmsg.indexOf( "access denied" ) >= 0 , "fsync not on admin should fail : " + tojson( x ) )

x = db._adminCommand( "fsync" )
assert( x.ok == 1 && x.numFiles > 0 , "fsync failed : " + tojson( x ) )

x = db._adminCommand( { "fsync" :1, lock:true } )
assert( ! x.ok , "lock should fail: " + tojson( x ) )


// write back stuff
// SERVER-4194

function countWritebacks( curop ) {
    print( "---------------" );
    var num = 0;
    for ( var i=0; i<curop.inprog.length; i++ ) {
        var q = curop.inprog[i].query;
        if ( q && q.writebacklisten ) {
            printjson( curop.inprog[i] );
            num++;
        }
    }
    return num;
}

x = db.currentOp();
assert.eq( 0 , countWritebacks( x ) , "without all");

x = db.currentOp( true );
y = countWritebacks( x )
assert( y == 1 || y == 2  , "with all: "  + y );





s.stop()
