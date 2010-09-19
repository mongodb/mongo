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
assert.eq( N , x.count , "total count" )
assert.eq( N / 2 , x.shards.shard0000.count , "count on shard0000" )
assert.eq( N / 2 , x.shards.shard0001.count , "count on shard0001" )

start = new Date()

print( "about to fork shell: " + Date() )
join = startParallelShell( "db.foo.find( function(){ x = \"\"; for ( i=0; i<10000; i++ ){ x+=i; } return true; } ).itcount()" )
print( "after forking shell: " + Date() )

function getMine( printInprog ){
    var inprog = db.currentOp().inprog;
    if ( printInprog )
        printjson( inprog )
    var mine = []
    for ( var x=0; x<inprog.length; x++ ){
        if ( inprog[x].query && inprog[x].query.$where ){
            mine.push( inprog[x] )
        }
    }
    return mine;
}

state = 0; // 0 = not found, 1 = killed, 
killTime = null;

for ( i=0; i<( 100* 1000 ); i++ ){
    mine = getMine( state == 0 && i > 20 );
    if ( state == 0 ){
        if ( mine.length == 0 ){
            sleep(1);
            continue;
        }
        state = 1;
        mine.forEach( function(z){ printjson( db.getSisterDB( "admin" ).killOp( z.opid ) ); }  )
        killTime = new Date()
    }
    else if ( state == 1 ){
        if ( mine.length == 0 ){
            state = 2;
            break;
        }
        sleep(1)
        continue;
    }
}

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

s.stop()
