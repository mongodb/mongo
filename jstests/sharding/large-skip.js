
admin = db.getSisterDB('admin');
test_db = db.getSisterDB('testdb');

coll1 = test_db.sharded_coll;
coll2 = test_db.not_sharded_coll;

admin.runCommand({ enableSharding : coll1.getDB() + "" });
admin.runCommand({ shardCollection : coll1 + "", key : { skey : 1 } });

admin.runCommand({ moveChunk : coll1, find : { skey : 1 }, to : 'set_test_1' })

function testit(coll){
    coll.remove();

    for (var sk = 0; sk < 10; sk++) {
        for (var id = 0; id < 100; id++) {
            coll.insert({ id : id, skey : sk});
        }
    }

    for (var sk = 0; sk < 3; sk++) {
        assert.eq(100, coll.find({ skey : sk }).itcount())
        assert.eq(90, coll.find({ skey : sk }).skip(10). itcount())
        assert.eq(5, coll.find({ skey : sk }).skip(10).limit(5).itcount())
        assert.eq(5, coll.find({ skey : sk }).sort({id: 1}).skip(10).limit(5).itcount())

    }
    assert.eq(10, coll.find({ id : 0 }).sort({skey: 1}).itcount())
    assert.eq(5, coll.find({ id : 0 }).sort({skey: 1}).skip(5).itcount())
    assert.eq(1, coll.find({ id : 0 }).sort({skey: 1}).skip(5).limit(1).itcount())
}

testit(coll1)
testit(coll2)

