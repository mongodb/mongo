/**
 * Test the behavior of $project on time-series collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_52,
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
})();