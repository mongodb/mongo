(function() {
    "use strict";
    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod failed to start.");
    let mydb = conn.getDB("test_ns1");

    const check = function(n, isNew) {
        var coll = mydb["x" + n];
        if (isNew) {
            assert.eq(0, coll.count(), "pop a: " + n);
            assert.writeOK(coll.insert({_id: n}));
        }
        assert.eq(1, coll.count(), "pop b: " + n);
        assert.eq(n, coll.findOne()._id, "pop c: " + n);
        return coll;
    };

    let max = 0;

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
    const its = max / 2;
    print("its: " + its);
    for (let i = 0; i < its; i++) {
        const x = Random.randInt(max);
        check(x, false).drop();
        checkall(x);
        check(x, true);
        if ((i + 1) % 20 == 0) {
            print(i + "/" + its);
        }
    }
    print("yay");

    MongoRunner.stopMongod(conn);
})();
