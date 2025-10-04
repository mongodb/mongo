// This test validates that map/reduce runs intermediate reduce steps in order to keep the
// in-memory state small. See SERVER-12949 for more details.

import {resultsEq} from "jstests/aggregation/extras/utils.js";

const testDb = db.getSiblingDB("MapReduceTestDB");
testDb.dropDatabase();

let coll = testDb.getCollection("mrInput");

// Insert 10 x 49 elements (10 i-s, 49 j-s)
let expectedOutColl = [];

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    for (var j = 1; j < 50; j++) {
        bulk.insert({idx: i, j: j});
    }
    expectedOutColl.push({_id: i, value: j - 1});
}
assert.commandWorked(bulk.execute());

function mapFn() {
    emit(this.idx, 1);
}
function reduceFn(key, values) {
    return Array.sum(values);
}

let out = coll.mapReduce(mapFn, reduceFn, {out: {replace: "mrOutput"}});

// Check the output is as expected
let outColl = testDb.getCollection("mrOutput").find().toArray();
assert(resultsEq(outColl, expectedOutColl));
