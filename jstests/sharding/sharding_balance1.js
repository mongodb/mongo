(function() {

var s = new ShardingTest({ name: "slow_sharding_balance1",
                           shards: 2,
                           mongos: 1,
                           other: { chunkSize: 1, enableBalancer : true } });

s.adminCommand( { enablesharding : "test" } );
s.ensurePrimaryShard('test', 'shard0001');

s.config.settings.find().forEach( printjson );

db = s.getDB( "test" );

bigString = "";
while ( bigString.length < 10000 )
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";

inserted = 0;
num = 0;
var bulk = db.foo.initializeUnorderedBulkOp();
while ( inserted < ( 20 * 1024 * 1024 ) ){
    bulk.insert({ _id: num++, s: bigString });
    inserted += bigString.length;
}
assert.writeOK(bulk.execute());

s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );
assert.lt( 20 , s.config.chunks.count()  , "setup2" );

function diff1(){
    var x = s.chunkCounts( "foo" );
    printjson( x );
    return Math.max( x.shard0000 , x.shard0001 ) - Math.min( x.shard0000 , x.shard0001 );
}

function sum(){
    var x = s.chunkCounts( "foo" );
    return x.shard0000 + x.shard0001;
}

assert.lt( 20 , diff1() , "big differential here" );
print( diff1() );

assert.soon( function(){
    var d = diff1();
    return d < 5;
// Make sure there's enough time here, since balancing can sleep for 15s or so between balances.
} , "balance didn't happen" , 1000 * 60 * 5 , 5000 );
    
var chunkCount = sum();
s.adminCommand( { removeshard: "shard0000" } );

assert.soon( function(){
    printjson(s.chunkCounts( "foo" ));
    s.config.shards.find().forEach(function(z){printjson(z);});
    return chunkCount == s.config.chunks.count({shard: "shard0001"});
} , "removeshard didn't happen" , 1000 * 60 * 3 , 5000 );

s.stop();

})();
