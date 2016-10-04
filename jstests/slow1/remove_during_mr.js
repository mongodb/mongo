// SERVER-15539
'use strict';

load('jstests/libs/parallelTester.js');

function client1() {
    Random.setRandomSeed();
    for (var i = 0; i < 1000; i++) {
        db.remove_during_mr.remove({rand: {$gte: Random.rand()}}, {justOne: true});
    }
}

function client2() {
    function mapper() {
        emit(this.key, 1);
    }

    function reducer() {
        return {};
    }

    for (var i = 0; i < 1000; i++) {
        var options = {out: {replace: 'bar'}, sort: {_id: -1}};

        db.remove_during_mr.mapReduce(mapper, reducer, options);
    }
}

// prepare some basic data for the collection
db.remove_during_mr.drop();

Random.setRandomSeed();
var bulk = db.remove_during_mr.initializeUnorderedBulkOp();
for (var i = 0; i < 3000; i++) {
    bulk.insert({i: i, key: Random.randInt(), rand: Random.rand()});
}
bulk.execute();

var threads = [];
for (var i = 0; i < 20; i++) {
    var t;

    if (i % 2 === 0) {
        t = new ScopedThread(client1);
    } else {
        t = new ScopedThread(client2);
    }

    threads.push(t);
    t.start();
}

threads.forEach(function(t) {
    t.join();
});
