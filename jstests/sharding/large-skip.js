var st = new ShardingTest({ shards : 2, mongos : 1});

var mongos = st.s0;
var shards = mongos.getDB( "config" ).shards.find().toArray();

var admin = mongos.getDB( "admin" );
var collSharded = mongos.getCollection( "testdb.collSharded" );
var collUnSharded = mongos.getCollection( "testdb.collUnSharded" );

assert( admin.runCommand({ enableSharding : collSharded.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : collSharded.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : collSharded + "", key : { skey : 1 } }).ok );
assert( admin.runCommand({ split : collSharded + "", middle : { skey : 0 } }).ok );
assert( admin.runCommand({ moveChunk : collSharded + "", find : { skey : 0 }, to : shards[1]._id }).ok );

function testSelectWithSkip(coll){
    jsTest.log( "test large-skip" );

    for (var sk = -5; sk < 5; sk++) {
        for (var id = 0; id < 100; id++) {
            coll.insert({ id : id, skey : sk});
        }
    }

    for (var sk = -2; sk < 2; sk++) {
        assert.eq(100, coll.find({ skey : sk }).itcount());
        assert.eq(90, coll.find({ skey : sk }).skip(10). itcount());
        assert.eq(5, coll.find({ skey : sk }).skip(10).limit(5).itcount());
        assert.eq(5, coll.find({ skey : sk }).sort({id: 1}).skip(10).limit(5).itcount());
    }
    assert.eq(10, coll.find({ id : 0 }).sort({skey: 1}).itcount());
    assert.eq(5, coll.find({ id : 0 }).sort({skey: 1}).skip(5).itcount());
    assert.eq(1, coll.find({ id : 0 }).sort({skey: 1}).skip(5).limit(1).itcount());
}

testSelectWithSkip(collSharded)
testSelectWithSkip(collUnSharded)

