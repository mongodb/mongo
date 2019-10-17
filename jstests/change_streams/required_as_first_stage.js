// Tests that the $changeStream stage can only be present as the first stage in the pipeline.
//
// The passthrough logic that bundles operations into transactions needs to be able identify change
// stream aggregations so as to avoid running them in a transaction, but that code would fail to
// recognize the intentionally malformed aggergations that we test here.
// @tags: [change_stream_does_not_expect_txns]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");       // For assertErrorCode.
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

const coll = assertDropAndRecreateCollection(db, "change_stream_required_as_first_stage");

assertErrorCode(coll, [{$match: {z: 34}}, {$changeStream: {}}], 40602);
assertErrorCode(coll, [{$indexStats: {}}, {$changeStream: {}}], 40602);
assertErrorCode(
    coll,
    [{$indexStats: {}}, {$changeStream: {}}, {$match: {test: "this is an extra stage"}}],
    40602);

let error = assert.throws(() => coll.aggregate([{$sort: {x: 1}}, {$changeStream: {}}]));
assert.contains(error.code, [40602], "Unexpected error: " + tojson(error));

error = assert.throws(
    () => coll.aggregate([{$sort: {x: 1}}, {$changeStream: {}}], {allowDiskUse: true}));
assert.contains(error.code, [40602], "Unexpected error: " + tojson(error));

error = assert.throws(() => coll.aggregate([{$group: {_id: "$x"}}, {$changeStream: {}}]));
assert.contains(error.code, [40602], "Unexpected error: " + tojson(error));

// This one has a different error code because of conflicting host type requirements: the $group
// needs to merge on a shard, but the $changeStream needs to merge on mongos. This doesn't
// happen for the $sort because the half of the $sort running on mongos is pre-sorted, and so
// won't need disk space.
error = assert.throws(
    () => coll.aggregate([{$group: {_id: "$x"}}, {$changeStream: {}}], {allowDiskUse: true}));
assert.contains(
    error.code, [40602, ErrorCodes.IllegalOperation], "Unexpected error: " + tojson(error));

// Test that a $changeStream stage is not allowed within a $facet stage.
assertErrorCode(coll, [{$facet: {testPipe: [{$changeStream: {}}]}}], 40600);
assertErrorCode(
    coll,
    [{
        $facet: {
            testPipe:
                [{$indexStats: {}}, {$changeStream: {}}, {$match: {test: "this is an extra stage"}}]
        }
    }],
    40600);
}());
