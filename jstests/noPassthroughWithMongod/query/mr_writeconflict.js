// SERVER-16262: Write-conflict during map-reduce operations

import {Thread} from "jstests/libs/parallelTester.js";

let makeDoc = function (keyLimit, valueLimit) {
    return {_id: ObjectId(), key: Random.randInt(keyLimit), value: Random.randInt(valueLimit)};
};

let main = function () {
    function mapper() {
        let obj = {};
        obj[this.value] = 1;
        emit(this.key, obj);
    }

    function reducer(key, values) {
        let res = {};

        values.forEach(function (obj) {
            Object.keys(obj).forEach(function (value) {
                if (!res.hasOwnProperty(value)) {
                    res[value] = 0;
                }
                res[value] += obj[value];
            });
        });

        return res;
    }

    for (let i = 0; i < 10; i++) {
        // Have all threads combine their results into the same collection
        let res = db.source.mapReduce(mapper, reducer, {out: {reduce: "dest"}});
        assert.commandWorked(res);
    }
};

Random.setRandomSeed();

let numDocs = 200;
let bulk = db.source.initializeUnorderedBulkOp();
let i;
for (i = 0; i < numDocs; ++i) {
    let doc = makeDoc(numDocs / 100, numDocs / 10);
    bulk.insert(doc);
}

let res = bulk.execute();
assert.commandWorked(res);
assert.eq(numDocs, res.nInserted);

db.dest.drop();
assert.commandWorked(db.createCollection("dest"));

let numThreads = 6;
let t = [];
for (i = 0; i < numThreads - 1; ++i) {
    t[i] = new Thread(main);
    t[i].start();
}

main();
for (i = 0; i < numThreads - 1; ++i) {
    t[i].join();
}
