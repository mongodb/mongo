// SERVER-2845 When skipping objects without loading them, they shouldn't be
// included in the nscannedObjects count.

var t = db.jstests_skip1;

// SERVER-13537: Ensure that combinations of skip and limit don't crash
// the server due to overflow.
t.drop();
for (var i = 0; i < 10; i++) {
    t.save({a: i});
}
assert.eq(9, t.find().sort({a: 1}).limit(2147483647).skip(1).itcount());
assert.eq(0, t.find().sort({a: 1}).skip(2147483647).limit(1).itcount());
