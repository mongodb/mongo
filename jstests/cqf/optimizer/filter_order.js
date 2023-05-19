(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_filter_order;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 10000; i++) {
    // "a" has the most ones, then "b", then "c".
    bulk.insert({a: (i % 2), b: (i % 3), c: (i % 4)});
}
assert.commandWorked(bulk.execute());

let res = coll.aggregate([{$match: {'a': {$eq: 1}, 'b': {$eq: 1}, 'c': {$eq: 1}}}]).toArray();
// TODO: verify plan that predicate on "c" is applied first (most selective), then "b", then "a".
}());
