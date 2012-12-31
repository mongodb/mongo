/** Simple test of sharding TTL collections.
 *  - Creates a new collection with a TTL index
 *  - Shards it, and moves one chunk containing half the docs to another shard.
 *  - Checks that both shards have TTL index, and docs get deleted on both shards.
 *  - Run the collMod command to update the expireAfterSeconds field. Check that more docs get
 *    deleted.
 */

// start up a new sharded cluster
var s = new ShardingTest({ shards : 2, mongos : 1});

var dbname = "testDB";
var coll = "ttl_sharded";
var ns = dbname + "." + coll;
t = s.getDB( dbname ).getCollection( coll );

// enable sharding of the collection. Only 1 chunk initially
s.adminCommand( { enablesharding : dbname } );
s.adminCommand( { shardcollection : ns , key: { _id : 1 } } );

// insert 24 docs, with timestamps at one hour intervals
var now = (new Date()).getTime();
for ( i=0; i<24; i++ ){
    var past = new Date( now - ( 3600 * 1000 * i ) );
    t.insert( {_id : i ,  x : past  } );
}
s.getDB( dbname ).getLastError();
assert.eq( t.count() , 24 , "initial docs not inserted");

// create the TTL index which delete anything older than ~5.5 hours
t.ensureIndex( { x : 1 } , { expireAfterSeconds : 20000 } );

// split chunk in half by _id, and move one chunk to the other shard
s.adminCommand( {split : ns , middle : {_id : 12 } } );
s.adminCommand( {moveChunk : ns , find : {_id : 0} , to : "shard0000" } )

// one shard will lose 12/12 docs, the other 6/12, so count will go
// from 24 -> 18 or 12 -> 6
assert.soon(
    function() {
        return t.count() < 7;
    }, "TTL index on x didn't delete enough" , 70 * 1000
);

// ensure that count ultimately ends up at 6
assert.eq( 0 , t.find( { x : { $lt : new Date( now - 20000000 ) } } ).count() );
assert.eq( 6 , t.count() );

// now lets check things explicily on each shard
var shard0 = s._connections[0].getDB( dbname );
var shard1 = s._connections[1].getDB( dbname );

print("Shard 0 coll stats:")
printjson( shard0.getCollection( coll ).stats() );
print("Shard 1 coll stats:")
printjson( shard1.getCollection( coll ).stats() );

// Check that TTL index (with expireAfterSeconds field) appears on both shards
var ttlIndexPattern = { "key": { "x" : 1 } , "ns": ns , "expireAfterSeconds" : 20000 };
assert.eq( 1 ,
           shard0.system.indexes.find( ttlIndexPattern ).count() ,
           "shard0 does not have TTL index");
assert.eq( 1 ,
           shard1.system.indexes.find( ttlIndexPattern ).count() ,
           "shard1 does not have TTL index");

// Check that the collMod command successfully updates the expireAfterSeconds field
s.getDB( dbname ).runCommand( { collMod : coll,
                                index : { keyPattern : {x : 1}, expireAfterSeconds : 10000} } );

var newTTLindex = { "key": { "x" : 1 } , "ns": ns , "expireAfterSeconds" : 10000 };
assert.eq( 1 ,
           shard0.system.indexes.find( newTTLindex ).count(),
           "shard0 index didn't get updated");
assert.eq( 1 ,
           shard1.system.indexes.find( newTTLindex ).count(),
           "shard1 index didn't get updated");

assert.soon(
    function() {
        return t.count() < 6;
    }, "new expireAfterSeconds value not taking effect" , 70 * 1000
);
assert.eq( 0 , t.find( { x : { $lt : new Date( now - 10000000 ) } } ).count() );
assert.eq( 3 , t.count() );

s.stop();

