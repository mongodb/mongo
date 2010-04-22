s = new ShardingTest( "auto1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.update1" , key : { key : 1 } } );

db = s.getDB( "test" )
coll = db.update1;

coll.insert({_id:1, key:1});

// these are upserts
coll.save({_id:2, key:2});
coll.save({_id:3, key:3});

assert.eq(coll.count(), 3, "count A")

// update existing using save()
coll.save({_id:1, key:1, other:1});

// update existing using update()
coll.update({_id:2}, {key:2, other:2});
//coll.update({_id:3, key:3}, {other:3}); //should add key to new object (doesn't work yet)
coll.update({_id:3}, {key:3, other:3});

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

s.stop()

