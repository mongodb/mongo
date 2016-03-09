function test() {
    "use strict";
    var t = db.update_serializability1;
    t.drop();

    var N = 100000;

    var bulk = t.initializeUnorderedBulkOp();
    for (var i = 0; i < N; i++) {
        bulk.insert({_id: i, a: i, b: N - i, x: 1, y: 1});
    }
    bulk.execute();

    t.ensureIndex({a: 1});
    t.ensureIndex({b: 1});

    var s1 = startParallelShell(
        "db.update_serializability1.update( { a : { $gte : 0 } }, { $set : { x : 2 } }, false, true );");
    var s2 = startParallelShell("db.update_serializability1.update( { b : { $lte : " + N +
                                " } }, { $set : { y : 2 } }, false, true );");

    s1();
    s2();

    // both operations should happen on every document
    assert.eq(N, t.find({x: 2, y: 2}).count());
}

if (db.serverStatus().storageEngine.name == 'mmapv1') {
    jsTest.log('skipping test on mmapv1');  // This is only guaranteed on other engines.
} else {
    test();
}
