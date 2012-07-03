
s = new ShardingTest( "migrateMemory" , 2 , 1 , 1 , { chunksize : 1 });

s.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

db = s.getDB( "test" ) 
t = db.foo

str = ""
while ( str.length < 10000 ){
    str += "asdasdsdasdasdasdas";
}

data = 0;
num = 0;
while ( data < ( 1024 * 1024 * 10 ) ){
    t.insert( { _id : num++ , s : str } )
    data += str.length
}

db.getLastError()

stats = s.chunkCounts( "foo" )
from = ""
to = ""
for ( x in stats ){
    if ( stats[x] == 0 )
        to = x
    else
        from = x
}

s.config.chunks.find().sort( { min : 1 } ).forEach( printjsononeline )

print( "from: " + from + " to: " + to )
printjson( stats )

ss = []

// Stop balancer for manual move
s.stopBalancer()

for ( var f = 0; f<num; f += ( 2 * num / t.stats().nchunks ) ){
    ss.push( s.getServer( "test" ).getDB( "admin" ).serverStatus() )
    print( f )
    
    //
    // Since we're moving chunks fast here, there's no guarantee that the to-shard will be done
    // before the from-shard with the migration.
    // 
    
    while( true ){
     
        try {
            s.adminCommand( { movechunk : "test.foo" , find : { _id : f } , to : to } )
        }
        catch( e ){
            
            // Not sure if thrown as string or obj
            if( tojson( e ).indexOf( "TO-shard" ) >= 0 ) continue
            
            printjson( e )
            throw e
        }
        
        break;
    }
    
}

// Re-enable balancer
s.setBalancer( true )

for ( i=0; i<ss.length; i++ )
    printjson( ss[i].mem );


s.stop()

