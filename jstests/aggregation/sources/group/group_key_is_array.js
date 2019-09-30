/**
 * Tests that a group _id with an array is evaluated whether it is at the top level or
 * nested.
 */
(function() {
"use strict";

const coll = db.group_with_arrays;
coll.drop();

assert.commandWorked(coll.insert([{x: null}, {y: null}, {x: null, y: null}]));

const arr_result = coll.aggregate([{$group: {_id: ["$x", "$y"]}}]);
const nested_result = coll.aggregate([{$group: {_id: {z: ["$x", "$y"]}}}]);

assert.eq(arr_result.toArray()[0]["_id"], nested_result.toArray()[0]["_id"]["z"]);
}());
