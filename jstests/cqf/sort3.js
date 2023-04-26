(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort3;
t.drop();

t.insert({a: 1, b: 1});
t.insert({a: 1, b: 2});
t.insert({a: 2, b: 1});
t.insert({a: 2, b: 2});

t.createIndex({a: 1, b: -1});

{
    // Make sure we pad compound intervals correctly when the sort is reversed.
    const resCollScan = t.find({a: {$gte: 2}}).sort({a: 1, b: -1}).hint({$natural: 1}).toArray();
    const resIndexScan = t.find({a: {$gte: 2}}).sort({a: 1, b: -1}).hint({a: 1, b: -1}).toArray();
    assert.eq(resCollScan, resIndexScan);
}
}());
