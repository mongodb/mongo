/**
 * Tests that $merge fails when the target collection is the aggregation collection.
 *
 * @tags: [assumes_unsharded_collection]
 */
(function() {
"use strict";

// For assertMergeFailsForAllModesWithCode.
load("jstests/aggregation/extras/merge_helpers.js");

const coll = db.name;
coll.drop();

const nDocs = 10;
for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}
assertMergeFailsForAllModesWithCode({source: coll, target: coll, errorCodes: 51188});
}());
