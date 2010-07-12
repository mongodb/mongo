s = new ShardingTest( "find_and_modify_sharded" , 2 );

s.adminCommand( { enablesharding : "test" } );
db = s.getDB( "test" );
primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

numObjs = 20;

s.adminCommand( { shardcollection : "test.stuff"  , key : {_id:1} } );

for (var i=0; i < numObjs; i++){
    db.stuff.insert({_id: i});
}

for (var i=0; i < numObjs; i+=2){
    s.adminCommand( { split: "test.stuff"  , middle : {_id: i} } );
}

for (var i=0; i < numObjs; i+=4){
    s.adminCommand( { movechunk : "test.stuff" , find : {_id: i} , to : seconday.getMongo().name } );
}

// update
for (var i=0; i < numObjs; i++){
    assert.eq(db.stuff.count({b:1}), i, "2 A");

    var out = db.stuff.findAndModify({query: {_id:i, b:null}, update: {$set: {b:1}}});
    assert.eq(out._id, i, "2 E");

    assert.eq(db.stuff.count({b:1}), i+1, "2 B");
}

// remove
for (var i=0; i < numObjs; i++){
    assert.eq(db.stuff.count(), numObjs - i, "3 A");
    assert.eq(db.stuff.count({_id: i}), 1, "3 B");

    var out = db.stuff.findAndModify({remove: true, query: {_id:i}});

    assert.eq(db.stuff.count(), numObjs - i - 1, "3 C");
    assert.eq(db.stuff.count({_id: i}), 0, "3 D");
    assert.eq(out._id, i, "3 E");
}

s.stop();
