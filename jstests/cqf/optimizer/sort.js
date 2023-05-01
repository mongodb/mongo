(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort;
t.drop();

assert.commandWorked(t.insert({_id: 1}));
assert.commandWorked(t.insert({_id: 2, x: null}));
assert.commandWorked(t.insert({_id: 3, x: []}));
assert.commandWorked(t.insert({_id: 4, x: [1, 2]}));
assert.commandWorked(t.insert({_id: 5, x: [10]}));
assert.commandWorked(t.insert({_id: 6, x: 4}));

const res = t.aggregate([{$unwind: '$x'}, {$sort: {'x': 1}}]).toArray();
assert.eq(4, res.length);
}());
