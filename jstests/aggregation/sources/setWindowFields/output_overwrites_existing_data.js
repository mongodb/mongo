/**
 * Tests that $setWindowFields will overwrite existing fields in the output document if there are
 * conflicts. This test was designed to confirm a behavior change from SERVER-63079.
 * @tags: [
 *   # $documents is not supported inside a $facet.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
(function() {
"use strict";

assert.commandWorked(db[jsTestName()].insert({dummy: 1}));

let windowResults = db.aggregate([
    {$documents: [{_id: 0, obj: {k1: "v1", k2: "v2"}}]},
    {$setWindowFields: {sortBy: {_id: 1}, output: {obj: {$last: {newSubObject: 1}}}}}
]);

// The 'newSubObject' should overwrite the existing 'k1' and 'k2' object.
assert.eq(windowResults.toArray(), [{_id: 0, obj: {newSubObject: 1}}]);

// Test that we can preserve the other fields by using a dotted notation and having the window
// function result in the target value (1 here) instead of an object literal.
windowResults = db.aggregate([
    {$documents: [{_id: 0, obj: {k1: "v1", k2: "v2"}}]},
    {$setWindowFields: {sortBy: {_id: 1}, output: {'obj.subPath': {$last: 1}}}}
]);

assert.eq(windowResults.toArray(), [{_id: 0, obj: {k1: "v1", k2: "v2", subPath: 1}}]);
})();
