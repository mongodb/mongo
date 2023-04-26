/**
 * Test that $lookup can generate documents larger than maximum BSON size, as long as only part of
 * such document is returned to the client.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.

const localColl = db.lookup_large_documents_local;
const foreignCollName = 'lookup_large_documents_foreign';
const foreignColl = db[foreignCollName];

localColl.drop();
foreignColl.drop();

const largeString = 'x'.repeat(10 * 1024 * 1024);
for (let i = 0; i < 8; ++i) {
    assert.commandWorked(foreignColl.insert({foreign: 1, largeField: largeString}));
}

assert.commandWorked(localColl.insert({local: 1}));

for (let preventProjectPushdown of [false, true]) {
    const pipeline = [{
        $lookup: {from: foreignCollName, localField: 'local', foreignField: 'foreign', as: 'result'}
    }];
    if (preventProjectPushdown) {
        pipeline.push({$_internalInhibitOptimization: {}});
    }
    pipeline.push({$project: {_id: 0, foo: {$add: ["$local", 2]}}});

    const results = localColl.aggregate(pipeline).toArray();

    assert(arrayEq(results, [{foo: 3}]),
           "Pipeline:\n" + tojson(pipeline) + "Actual results:\n" + tojson(results));
}
}());
