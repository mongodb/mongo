(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_array_size;
t.drop();

assert.commandWorked(t.insert({a: [1, 2, 3, 4]}));
assert.commandWorked(t.insert({a: [2, 3, 4]}));
assert.commandWorked(t.insert({a: [2, 2]}));
assert.commandWorked(t.insert({a: 2}));
assert.commandWorked(t.insert({a: [1, 3]}));
assert.commandWorked(t.insert({a: [{b: [1, 3]}]}));
assert.commandWorked(t.insert({a: [{b: [[1, 3]]}]}));

let res = t.explain("executionStats").aggregate([{$match: {a: {$size: 2}}}]);
assert.eq(2, res.executionStats.nReturned);

res = t.explain("executionStats").aggregate([{$project: {b: {$size: '$a'}}}, {$match: {b: 2}}]);
assert.eq(2, res.executionStats.nReturned);

res = t.explain("executionStats").aggregate([{$match: {'a.b': {$size: 2}}}]);
assert.eq(1, res.executionStats.nReturned);
}());
