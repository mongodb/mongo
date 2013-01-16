// Test simple updates issued through mongos. Updates have different constraints through mongos,
// since shard key is immutable.

s = new ShardingTest( "auto1" , 2 , 1 , 1 );

s.adminCommand( { enablesharding : "test" } );
// repeat same tests with hashed shard key, to ensure identical behavior
s.adminCommand( { shardcollection : "test.update0" , key : { key : 1 } } );
s.adminCommand( { shardcollection : "test.update1" , key : { key : "hashed" } } );

db = s.getDB( "test" )
for(i=0; i < 2; i++){
    coll = db.getCollection("update" + i);

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

    // do a replacement-style update which queries the shard key and keeps it constant
    coll.save( {_id:4, key:4} );
    coll.update({key:4}, {key:4, other:4});
    assert.eq( coll.find({key:4, other:4}).count() , 1 , 'replacement update error');
    coll.remove( {_id:4} )

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
}

s.stop()

