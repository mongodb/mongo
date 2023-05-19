(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort2;
t.drop();

t.save({a: 0});
t.save({a: 1});
t.save({a: 2});

t.createIndex({a: 1});

{
    // Make sure we are reversing bounds correctly during lowering.
    const resCollScan = t.find({a: {$gte: 1}}).sort({a: -1}).hint({$natural: 1}).toArray();
    const resIndexScan = t.find({a: {$gte: 1}}).sort({a: -1}).hint({a: 1}).toArray();
    assert.eq(resCollScan, resIndexScan);
}
}());
