
load("jstests/libs/slow_weekly_util.js");

testServer = new SlowWeeklyMongod("ns1");
mydb = testServer.getDB("test_ns1");

check = function(n, isNew) {
    var coll = mydb["x" + n];
    if (isNew) {
        assert.eq(0, coll.count(), "pop a: " + n);
        coll.insert({_id: n});
    }
    assert.eq(1, coll.count(), "pop b: " + n);
    assert.eq(n, coll.findOne()._id, "pop c: " + n);
    return coll;
};

max = 0;

for (; max < 1000; max++) {
    check(max, true);
}

function checkall(removed) {
    for (var i = 0; i < max; i++) {
        if (removed == i) {
            assert.eq(0, mydb["x" + i].count(), "should be 0 : " + removed);
        } else {
            check(i, false);
        }
    }
}

checkall();

Random.srand(123124);
its = max / 2;
print("its: " + its);
for (i = 0; i < its; i++) {
    x = Random.randInt(max);
    check(x, false).drop();
    checkall(x);
    check(x, true);
    if ((i + 1) % 20 == 0) {
        print(i + "/" + its);
    }
}
print("yay");

mydb.dropDatabase();
