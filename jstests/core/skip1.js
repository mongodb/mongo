// SERVER-2845 When skipping objects without loading them, they shouldn't be
// included in the nscannedObjects count.

var t = db.jstests_skip1;

if (0) {  // SERVER-2845
    t.drop();

    t.ensureIndex({a: 1});
    t.save({a: 5});
    t.save({a: 5});
    t.save({a: 5});

    assert.eq(3, t.find({a: 5}).skip(2).explain().nscanned);
    assert.eq(1, t.find({a: 5}).skip(2).explain().nscannedObjects);
}

// SERVER-13537: Ensure that combinations of skip and limit don't crash
// the server due to overflow.
t.drop();
for (var i = 0; i < 10; i++) {
    t.save({a: i});
}
assert.eq(9, t.find().sort({a: 1}).limit(2147483647).skip(1).itcount());
assert.eq(0, t.find().sort({a: 1}).skip(2147483647).limit(1).itcount());

if (!db.getMongo().useReadCommands()) {
    // If we're using OP_QUERY/OP_GET_MORE reads rather than find/getMore command, then the skip and
    // limit fields must fit inside a 32-bit signed integer.
    assert.throws(function() {
        assert.eq(0, t.find().sort({a: 1}).skip(2147483648).itcount());
    });
    assert.throws(function() {
        assert.eq(0, t.find().sort({a: 1}).limit(2147483648).itcount());
    });
}
