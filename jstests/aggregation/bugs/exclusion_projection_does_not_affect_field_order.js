// Test that excluding a non-existent field does not affect the field order produced by subsequent
// changes.
//
// This is designed as a regression test for SERVER-37791.
(function() {
"use strict";

const coll = db.exclusion_projection_does_not_affect_field_order;
coll.drop();

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.insert({_id: 2, c: 1}));
assert.commandWorked(coll.insert({_id: 3, y: 1, z: 1}));

// We expect $addFields to retain the position of pre-existing fields, and then append new fields in
// the order that they are specified in the query. This rule should not be impacted by the presence
// of a preceding exclusion projection.
assert.eq(
    [
        {_id: 1, x: 3, y: 4, b: 5, c: 6, a: 7},
        // Here "c" retains the position that it had prior to being excluded.
        {_id: 2, c: 6, x: 3, y: 4, b: 5, a: 7},
        {_id: 3, y: 4, z: 1, x: 3, b: 5, c: 6, a: 7}
    ],
    coll.aggregate([
            {$project: {b: 0, c: 0}},
            {$addFields: {x: 3, y: 4, b: 5, c: 6, a: 7}},
            {$sort: {_id: 1}}
        ])
        .toArray());
}());
