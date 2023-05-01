(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_partial_index;
t.drop();

assert.commandWorked(t.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(t.insert({a: 3, b: 2, c: 1}));
assert.commandWorked(t.insert({a: 3, b: 3, c: 1}));
assert.commandWorked(t.insert({a: 3, b: 3, c: 2}));
assert.commandWorked(t.insert({a: 4, b: 3, c: 2}));
assert.commandWorked(t.insert({a: 5, b: 5, c: 2}));

for (let i = 0; i < 40; i++) {
    assert.commandWorked(t.insert({a: i + 10, b: i + 10, c: i + 10}));
}

assert.commandWorked(t.createIndex({'a': 1}, {partialFilterExpression: {'b': 2}}));

// TODO: verify with explain the plan should use the index.
let res = t.aggregate([{$match: {'a': 3, 'b': 2}}]).toArray();
assert.eq(1, res.length);

// TODO: verify with explain the plan should not use the index.
res = t.aggregate([{$match: {'a': 3, 'b': 3}}]).toArray();
assert.eq(2, res.length);
}());
