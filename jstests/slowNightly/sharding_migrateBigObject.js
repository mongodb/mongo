
var shardA = startMongodEmpty("--shardsvr", "--port", 30001, "--dbpath", "/data/db/migrateBigger0", "--nopreallocj");
var shardB = startMongodEmpty("--shardsvr", "--port", 30002, "--dbpath", "/data/db/migrateBigger1", "--nopreallocj");
var config = startMongodEmpty("--configsvr", "--port", 29999, "--dbpath", "/data/db/migrateBiggerC", "--nopreallocj");

var mongos = startMongos({ port : 30000, configdb : "localhost:29999" })

var admin = mongos.getDB("admin")

admin.runCommand({ addshard : "localhost:30001" })
admin.runCommand({ addshard : "localhost:30002" })

db = mongos.getDB("test");
var coll = db.getCollection("stuff")

var data = "x"
var nsq = 16
var n = 255

for( var i = 0; i < nsq; i++ ) data += data

dataObj = {}
for( var i = 0; i < n; i++ ) dataObj["data-" + i] = data

for( var i = 0; i < 40; i++ ) {
        if(i != 0 && i % 10 == 0) printjson( coll.stats() )
        coll.save({ data : dataObj })
}
db.getLastError();

assert.eq( 40 , coll.count() , "prep1" );

printjson( coll.stats() )

admin.runCommand({ enablesharding : "" + coll.getDB() })

admin.printShardingStatus()

admin.runCommand({ shardcollection : "" + coll, key : { _id : 1 } })

assert.lt( 5 , mongos.getDB( "config" ).chunks.find( { ns : "test.stuff" } ).count() , "not enough chunks" );

assert.soon( 
    function(){
        
        // On *extremely* slow or variable systems, we've seen migrations fail in the critical section and
        // kill the server.  Do an explicit check for this. SERVER-8781
        // TODO: Remove once we can better specify what systems to run what tests on.
        try { 
            assert.eq(null, shardA.getDB("admin").getLastError());
            assert.eq(null, shardB.getDB("admin").getLastError());
        }
        catch(e) {
            print("An error occurred contacting a shard during balancing," +
                  " this may be due to slow disk I/O, aborting test.");
            throw e;
        }
         
        res = mongos.getDB( "config" ).chunks.group( { cond : { ns : "test.stuff" } , 
                                                       key : { shard : 1 }  , 
                                                       reduce : function( doc , out ){ out.nChunks++; } , 
                                                       initial : { nChunks : 0 } } );
        
        printjson( res );
        return res.length > 1 && Math.abs( res[0].nChunks - res[1].nChunks ) <= 3;

    } , 
    "never migrated" , 10 * 60 * 1000 , 1000 );

stopMongod( 30000 );
stopMongod( 29999 );
stopMongod( 30001 );
stopMongod( 30002 );


