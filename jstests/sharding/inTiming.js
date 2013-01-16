// Check that shard selection does not take a really long time on $in queries: SERVER-4745

s = new ShardingTest( 'sharding_inqueries', 3, 0, 1, {chunksize:1});

db = s.getDB( 'test' );

s.adminCommand( { enablesharding: 'test' } );
s.adminCommand( { shardcollection: 'test.foo', key: { a:1, b:1 } } );

var lst = [];
for (var i = 0; i < 500; i++) { lst.push(i); }

/*
* Time how long it takes to do $in querys on a sharded and unsharded collection.
* There is no data in either collection, so the query time is coming almost
* entirely from the code that selects which shard(s) to send the query to.
*/
unshardedQuery = function() {db.bar.find({a:{$in:lst}, b:{$in:lst}}).itcount()};
shardedQuery = function() {db.foo.find({a:{$in:lst}, b:{$in:lst}}).itcount()};
// Run queries a few times to warm memory
for (var i = 0; i < 3; i++) {
    unshardedQuery();
    shardedQuery();
}

unshardedTime = Date.timeFunc(unshardedQuery , 5);
shardedTime = Date.timeFunc(shardedQuery, 5);

print("Unsharded $in query ran in " + unshardedTime);
print("Sharded $in query ran in " + shardedTime);
assert(unshardedTime * 10 > shardedTime, "Sharded query is more than 10 times as slow as unsharded query");

s.stopBalancer();

db.adminCommand({split : "test.foo", middle : { a:1, b:10}});
db.adminCommand({split : "test.foo", middle : { a:3, b:0}});

db.adminCommand({moveChunk : "test.foo", find : {a:1, b:0}, to : "shard0000", _waitForDelete : true});
db.adminCommand({moveChunk : "test.foo", find : {a:1, b:15}, to : "shard0001", _waitForDelete : true});
db.adminCommand({moveChunk : "test.foo", find : {a:3, b:15}, to : "shard0002", _waitForDelete : true});

// Now make sure we get the same results from sharded and unsharded query.

for (var i = 0; i < 20; i++) {
    db.foo.save({a:1, b:i});
    db.foo.save({a:2, b:i});
    db.foo.save({a:3, b:i});
    db.foo.save({a:4, b:i});
}

db.printShardingStatus();

assert.eq(6, db.foo.find({a : {$in : [1, 2]}, b : {$in : [0, 3, 5]}}).itcount());
assert.eq(14, db.foo.find({a : {$in : [1, 2]}, b : {$in : [0, 3, 5, 10, 11, 15, 19]}}).itcount());
assert.eq(28, db.foo.find({a : {$in : [1, 2, 3, 4]}, b : {$in : [0, 3, 5, 10, 11, 15, 19]}}).itcount());
assert.eq(14, db.foo.find({a : {$in : [3, 4]}, b : {$in : [0, 3, 5, 10, 11, 15, 19]}}).itcount());

s.stop();