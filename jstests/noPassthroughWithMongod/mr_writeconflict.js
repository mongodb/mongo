// SERVER-16262: Write-conflict during map-reduce operations
(function() {
    "use strict";

    load('jstests/libs/parallelTester.js');

    var makeDoc = function(keyLimit, valueLimit) {
        return {_id: ObjectId(), key: Random.randInt(keyLimit), value: Random.randInt(valueLimit)};
    };

    var main = function() {

        function mapper() {
            var obj = {};
            obj[this.value] = 1;
            emit(this.key, obj);
        }

        function reducer(key, values) {
            var res = {};

            values.forEach(function(obj) {
                Object.keys(obj).forEach(function(value) {
                    if (!res.hasOwnProperty(value)) {
                        res[value] = 0;
                    }
                    res[value] += obj[value];
                });
            });

            return res;
        }

        for (var i = 0; i < 10; i++) {
            // Have all threads combine their results into the same collection
            var res = db.source.mapReduce(mapper, reducer, {out: {reduce: 'dest'}});
            assert.commandWorked(res);
        }
    };

    Random.setRandomSeed();

    var numDocs = 200;
    var bulk = db.source.initializeUnorderedBulkOp();
    var i;
    for (i = 0; i < numDocs; ++i) {
        var doc = makeDoc(numDocs / 100, numDocs / 10);
        bulk.insert(doc);
    }

    var res = bulk.execute();
    assert.writeOK(res);
    assert.eq(numDocs, res.nInserted);

    db.dest.drop();
    assert.commandWorked(db.createCollection('dest'));

    var numThreads = 6;
    var t = [];
    for (i = 0; i < numThreads - 1; ++i) {
        t[i] = new ScopedThread(main);
        t[i].start();
    }

    main();
    for (i = 0; i < numThreads - 1; ++i) {
        t[i].join();
    }
}());
