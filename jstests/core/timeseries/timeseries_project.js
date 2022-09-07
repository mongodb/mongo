/**
 * Test the behavior of $project on time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const coll = db.timeseries_project;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

const docDate = ISODate();

assert.commandWorked(coll.insert({_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}));

// Check that measurements being unpacked don't overwrite metadata projection pushdown fields.
let result =
    coll.aggregate([{
            $project:
                {a: 1, b: "$meta", c: {$multiply: [2, "$meta"]}, d: {$multiply: [2, "$meta"]}}
        }])
        .toArray();
assert.docEq(result, [{_id: 0, a: {b: 1}, b: 4, c: 8, d: 8}]);

// Same as above, but keep the rest of the document.
result = coll.aggregate([{$set: {b: "$meta"}}]).toArray();
assert.docEq(result, [{_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 4, c: [{}, {}]}]);

// Check that nested meta project is not overwritten by the unpacked value.
result = coll.aggregate([{$project: {"a.b": "$meta"}}]).toArray();
assert.docEq(result, [{_id: 0, a: {b: 4}}]);

// Check that meta project pushed down writes to each value in an array.
result = coll.aggregate([{$project: {"c.a": "$meta"}}]).toArray();
assert.docEq(result, [{_id: 0, c: [{a: 4}, {a: 4}]}]);

// Replace meta field with unpacked field.
result = coll.aggregate([{$project: {"meta": "$b"}}]).toArray();
assert.docEq(result, [{_id: 0, meta: 3}]);

// Replace meta field with time field.
result = coll.aggregate([{$project: {"meta": "$time"}}]).toArray();
assert.docEq(result, [{_id: 0, meta: docDate}]);

// Replace meta field with constant.
result = coll.aggregate([{$project: {"meta": {$const: 5}}}]).toArray();
assert.docEq(result, [{_id: 0, meta: 5}]);

// Make sure the time field can be overwritten by the meta field correctly.
result = coll.aggregate([{$set: {time: "$meta"}}]).toArray();
assert.docEq(result, [{_id: 0, time: 4, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}]);

// Check that the time field can be overwritten by the an unpacked field correctly.
result = coll.aggregate([{$set: {time: "$b"}}]).toArray();
assert.docEq(result, [{_id: 0, time: 3, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}]);

// Make sure the time field can be overwritten by a constant correctly.
result = coll.aggregate([{$project: {time: {$const: 5}}}]).toArray();
assert.docEq(result, [{_id: 0, time: 5}]);

// Test that a pushed down meta field projection can correctly be excluded.
result = coll.aggregate([{$set: {b: "$meta"}}, {$unset: "a"}]).toArray();
assert.docEq(result, [{_id: 0, time: docDate, meta: 4, b: 4, c: [{}, {}]}]);

// Exclude behavior for time field.
result = coll.aggregate([{$set: {b: "$time"}}, {$unset: "a"}]).toArray();
assert.docEq(result, [{_id: 0, time: docDate, meta: 4, b: docDate, c: [{}, {}]}]);

// Exclude behavior for consecutive projects.
result = coll.aggregate([{$set: {b: "$meta"}}, {$unset: "meta"}]).toArray();
assert.docEq(result, [{_id: 0, time: docDate, a: {b: 1}, b: 4, c: [{}, {}]}]);

// Test that an exclude does not overwrite meta field pushdown.
result = coll.aggregate([{$unset: "b"}, {$set: {b: "$meta"}}]).toArray();
assert.docEq(result, [{_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 4, c: [{}, {}]}]);

// Test that a field reference in a projection refers to the stage's input document
// rather than another field with the same name in the projection.
(function() {
const regColl = db.timeseries_project_reg;
regColl.drop();

const tsColl = db.timeseries_project_ts;
tsColl.drop();
assert.commandWorked(
    db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time', metaField: 'x'}}));

const doc = {
    time: new Date("2019-10-11T14:39:18.670Z"),
    x: 5,
    a: 3,
};
assert.commandWorked(tsColl.insert(doc));
assert.commandWorked(regColl.insert(doc));

// Test $project.
let pipeline = [{$project: {_id: 0, a: "$x", b: "$a"}}];
let tsDoc = tsColl.aggregate(pipeline).toArray();
let regDoc = regColl.aggregate(pipeline).toArray();
assert.docEq(tsDoc, regDoc);

// Test $addFields.
pipeline = [{$addFields: {a: "$x", b: "$a"}}, {$project: {_id: 0}}];
tsDoc = tsColl.aggregate(pipeline).toArray();
regDoc = regColl.aggregate(pipeline).toArray();
assert.docEq(tsDoc, regDoc);
})();
})();
