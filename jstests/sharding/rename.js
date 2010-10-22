s = new ShardingTest( "rename" , 2 , 1 , 1 );
db = s.getDB( "test" );

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

db.foo.insert({_id:3});
db.foo.renameCollection('bar', true);
assert.isnull(db.getLastError(), '3.0');
assert.eq(db.bar.findOne(), {_id:3}, '3.1');
assert.eq(db.bar.count(), 1, '3.2');
assert.eq(db.foo.count(), 0, '3.3');

s.stop()