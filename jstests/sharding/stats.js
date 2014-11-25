s = new ShardingTest( "stats" , 2 , 1 , 1 );
s.adminCommand( { enablesharding : "test" } );

a = s._connections[0].getDB( "test" );
b = s._connections[1].getDB( "test" );

db = s.getDB( "test" );

function numKeys(o){
    var num = 0;
    for (var x in o)
        num++;
    return num;
}

// ---------- load some data -----

// need collections sharded before and after main collection for proper test
s.adminCommand( { shardcollection : "test.aaa" , key : { _id : 1 } } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } ); // this collection is actually used
s.adminCommand( { shardcollection : "test.zzz" , key : { _id : 1 } } );


N = 10000;
s.adminCommand( { split : "test.foo" , middle : { _id : N/2 } } )
s.adminCommand({ moveChunk: "test.foo", find: { _id: 3 },
    to: s.getNonPrimaries("test")[0], _waitForDelete: true });

for ( i=0; i<N; i++ )
    db.foo.insert( { _id : i } )
db.getLastError();

x = db.foo.stats();
assert.eq( N , x.count , "coll total count expected" )
assert.eq( db.foo.count() , x.count , "coll total count match" )
assert.eq( 2 , x.nchunks , "coll chunk num" )
assert.eq( 2 , numKeys(x.shards) , "coll shard num" )
assert.eq( N / 2 , x.shards.shard0000.count , "coll count on shard0000 expected" )
assert.eq( N / 2 , x.shards.shard0001.count , "coll count on shard0001 expected" )
assert.eq( a.foo.count() , x.shards.shard0000.count , "coll count on shard0000 match" )
assert.eq( b.foo.count() , x.shards.shard0001.count , "coll count on shard0001 match" )


a_extras = a.stats().objects - a.foo.count(); // things like system.namespaces and system.indexes
b_extras = b.stats().objects - b.foo.count(); // things like system.namespaces and system.indexes
print("a_extras: " + a_extras);
print("b_extras: " + b_extras);

x = db.stats();

//dbstats uses Future::CommandResult so raw output uses connection strings not shard names
shards = Object.keySet(x.raw);

assert.eq( N + (a_extras + b_extras) , x.objects , "db total count expected" )
assert.eq( 2 , numKeys(x.raw) , "db shard num" )
assert.eq( (N / 2) + a_extras, x.raw[shards[0]].objects , "db count on shard0000 expected" )
assert.eq( (N / 2) + b_extras, x.raw[shards[1]].objects , "db count on shard0001 expected" )
assert.eq( a.stats().objects , x.raw[shards[0]].objects , "db count on shard0000 match" )
assert.eq( b.stats().objects , x.raw[shards[1]].objects , "db count on shard0001 match" )

/* Test db.stat() and db.collection.stat() scaling */

/* Helper functions */
function statComp(stat, stat_scaled, scale) {
    /* Because of loss of floating point precision, do not check exact equality */
    assert(((stat_scaled - 2) <= (stat / scale)) &&
           ((stat / scale) <= (stat_scaled + 2)));
}

function dbStatComp(stat_obj, stat_obj_scaled, scale) {
    statComp(stat_obj.dataSize, stat_obj_scaled.dataSize, scale);
    statComp(stat_obj.storageSize, stat_obj_scaled.storageSize, scale);
    statComp(stat_obj.indexSize, stat_obj_scaled.indexSize, scale);
    statComp(stat_obj.fileSize, stat_obj_scaled.fileSize, scale);
    /* avgObjSize not scaled.  See SERVER-7347 */
    statComp(stat_obj.avgObjSize, stat_obj_scaled.avgObjSize, 1);
}

function collStatComp(stat_obj, stat_obj_scaled, scale, mongos) {
    statComp(stat_obj.size, stat_obj_scaled.size, scale);
    statComp(stat_obj.storageSize, stat_obj_scaled.storageSize, scale);
    statComp(stat_obj.totalIndexSize, stat_obj_scaled.totalIndexSize, scale);
    statComp(stat_obj.avgObjSize, stat_obj_scaled.avgObjSize, scale);
    /* lastExtentSize doesn't exist in mongos level collection stats */
    if (!mongos) {
        statComp(stat_obj.lastExtentSize, stat_obj_scaled.lastExtentSize, scale);
    }
}

/* db.stats() tests */
db_not_scaled = db.stats();
db_scaled_512 = db.stats(512);
db_scaled_1024 = db.stats(1024);

for (var shard in db_not_scaled.raw) {
    dbStatComp(db_not_scaled.raw[shard], db_scaled_512.raw[shard], 512);
    dbStatComp(db_not_scaled.raw[shard], db_scaled_1024.raw[shard], 1024);
}

dbStatComp(db_not_scaled, db_scaled_512, 512);
dbStatComp(db_not_scaled, db_scaled_1024, 1024);

/* db.collection.stats() tests */
coll_not_scaled = db.foo.stats();
coll_scaled_512 = db.foo.stats(512);
coll_scaled_1024 = db.foo.stats(1024);

for (var shard in coll_not_scaled.raw) {
    collStatComp(coll_not_scaled.raw[shard], coll_scaled_512.raw[shard], 512, false);
    collStatComp(coll_not_scaled.raw[shard], coll_scaled_1024.raw[shard], 1024, false);
}

collStatComp(coll_not_scaled, coll_scaled_512, 512, true);
collStatComp(coll_not_scaled, coll_scaled_1024, 1024, true);

s.stop()
