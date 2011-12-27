// Check that shard selection does not assert for certain unsatisfiable queries.  SERVER-4554

s = new ShardingTest( 'shard_index2', 2, 0, 1 );

db = s.admin._mongo.getDB( 'test' );
c = db[ 'foo' ];
c.drop();

s.adminCommand( { enablesharding: '' + db } );
s.adminCommand( { shardcollection: '' + c, key: { a:1,b:1 } } );

assert.eq( 0, c.find({a:1}).itcount() );
assert.eq( 0, c.find({a:1,b:1}).itcount() );
assert.eq( 0, c.find({a:{$gt:4,$lt:2}}).itcount() );
if ( 0 ) { // SERVER-4554
assert.eq( 0, c.find({a:1,b:{$gt:4,$lt:2}}).itcount() );
assert.eq( 0, c.find({a:{$gt:0,$lt:2},b:{$gt:4,$lt:2}}).itcount() );
}
assert.eq( 0, c.find({b:{$gt:4,$lt:2}}).itcount() );
assert.eq( 0, c.find({a:{$in:[]}}).itcount() );
if ( 0 ) { // SERVER-4554
assert.eq( 0, c.find({a:1,b:{$in:[]}}).itcount() );
}

assert.eq( 0, c.find({$or:[{a:{$gt:0,$lt:10}},{a:12}]}).itcount() );
assert.eq( 0, c.find({$or:[{a:{$gt:0,$lt:10}},{a:5}]}).itcount() );
assert.eq( 0, c.find({$or:[{a:1,b:{$gt:0,$lt:10}},{a:1,b:5}]}).itcount() );
