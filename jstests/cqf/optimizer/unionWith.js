(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

load("jstests/aggregation/extras/utils.js");

const collA = db.collA;
collA.drop();

const collB = db.collB;
collB.drop();

assert.commandWorked(collA.insert({_id: 0, a: 1}));
assert.commandWorked(collB.insert({_id: 0, a: 2}));

let res = collA.aggregate([{$unionWith: "collB"}]).toArray();
assert.eq(2, res.length);
assert.eq([{_id: 0, a: 1}, {_id: 0, a: 2}], res);

// Test a filter after the union which can be pushed down to each branch.
res = collA.aggregate([{$unionWith: "collB"}, {$match: {a: {$lt: 2}}}]).toArray();
assert.eq(1, res.length);
assert.eq([{_id: 0, a: 1}], res);

// Test a non-simple inner pipeline.
res = collA.aggregate([{$unionWith: {coll: "collB", pipeline: [{$match: {a: 2}}]}}]).toArray();
assert.eq(2, res.length);
assert.eq([{_id: 0, a: 1}, {_id: 0, a: 2}], res);

// Test a union with non-existent collection.
res = collA.aggregate([{$unionWith: "non_existent"}]).toArray();
assert.eq(1, res.length);
assert.eq([{_id: 0, a: 1}], res);

// Test union alongside projections. This is meant to test the pipeline translation logic that adds
// a projection to the inner pipeline when necessary.
res = collA.aggregate([{$project: {_id: 0, a: 1}}, {$unionWith: "collB"}]).toArray();
assert.eq(2, res.length);
assert.eq([{a: 1}, {_id: 0, a: 2}], res);

res = collA.aggregate([{$unionWith: {coll: "collB", pipeline: [{$project: {_id: 0, a: 1}}]}}])
          .toArray();
assert.eq(2, res.length);
assert.eq([{_id: 0, a: 1}, {a: 2}], res);

res = collA.aggregate([{$unionWith: "collB"}, {$project: {_id: 0, a: 1}}]).toArray();
assert.eq(2, res.length);
assert.eq([{a: 1}, {a: 2}], res);
}());
