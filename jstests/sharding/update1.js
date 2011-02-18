s = new ShardingTest( "auto1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.update1" , key : { key : 1 } } );

db = s.getDB( "test" )
coll = db.update1;

coll.insert({_id:1, key:1});

// these are both upserts
coll.save({_id:2, key:2});
coll.update({_id:3, key:3}, {$set: {foo: 'bar'}}, {upsert: true});

assert.eq(coll.count(), 3, "count A")
assert.eq(coll.findOne({_id:3}).key, 3 , "findOne 3 key A")
assert.eq(coll.findOne({_id:3}).foo, 'bar' , "findOne 3 foo A")

// update existing using save()
coll.save({_id:1, key:1, other:1});

// update existing using update()
coll.update({_id:2}, {key:2, other:2});
coll.update({_id:3}, {key:3, other:3});

coll.update({_id:3, key:3}, {other:4});
assert.eq(db.getLastErrorObj().code, 12376, 'bad update error');

assert.eq(coll.count(), 3, "count B")
coll.find().forEach(function(x){
    assert.eq(x._id, x.key, "_id == key");
    assert.eq(x._id, x.other, "_id == other");
});


coll.update({_id:1, key:1}, {$set: {key:2}});
err = db.getLastErrorObj();
assert.eq(coll.findOne({_id:1}).key, 1, 'key unchanged');
assert.eq(err.code, 13123, 'key error code 1');
assert.eq(err.code, 13123, 'key error code 2');

coll.update({_id:1, key:1}, {$set: {foo:2}});
assert.isnull(db.getLastError(), 'getLastError reset');

coll.update( { key : 17 } , { $inc : { x : 5 } } , true  );
assert.eq( 5 , coll.findOne( { key : 17 } ).x , "up1" )

coll.update( { key : 18 } , { $inc : { x : 5 } } , true , true );
assert.eq( 5 , coll.findOne( { key : 18 } ).x , "up2" )


s.stop()

