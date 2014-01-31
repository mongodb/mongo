var rsOpt = { oplogSize: 10 };
var s = new ShardingTest( { name:"rename", shards:2, verbose:1, mongos:1, rs:rsOpt } );
var db = s.getDB( "test" );
var replTest = s.rs0;

db.foo.insert({_id:1});
db.foo.renameCollection('bar');
assert.isnull(db.getLastError(), '1.0');
assert.eq(db.bar.findOne(), {_id:1}, '1.1');
assert.eq(db.bar.count(), 1, '1.2');
assert.eq(db.foo.count(), 0, '1.3');

db.foo.insert({_id:2});
db.foo.renameCollection('bar', true);
assert.isnull(db.getLastError(), '2.0');
assert.eq(db.bar.findOne(), {_id:2}, '2.1');
assert.eq(db.bar.count(), 1, '2.2');
assert.eq(db.foo.count(), 0, '2.3');

s.adminCommand( { enablesharding : "test" } );

jsTest.log("Testing write concern (1)");

db.foo.insert({_id:3});
db.foo.renameCollection('bar', true);
var ans = db.runCommand({getLastError:1, w:3});
printjson(ans);
assert.isnull(ans.err, '3.0');

assert.eq(db.bar.findOne(), {_id:3}, '3.1');
assert.eq(db.bar.count(), 1, '3.2');
assert.eq(db.foo.count(), 0, '3.3');

// Ensure write concern works by shutting down 1 node in a replica set shard.
jsTest.log("Testing write concern (2)");
replTest.stop(replTest.getNodeId(replTest.getSecondary()));

db.foo.insert({_id:4});
printjson(db.foo.renameCollection('bar', true));
ans = db.runCommand({getLastError:1, w:3, wtimeout:5000});
printjson(ans);
assert.eq(ans.err, "timeout", '4.0');

s.stop();
