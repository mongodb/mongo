// Tests that text search requires a text index.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For "assertErrorCode".
load("jstests/libs/fixture_helpers.js");      // For isSharded.

const coll = db.coll;
const from = db.from;

coll.drop();
from.drop();

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(from) && !isShardedLookupEnabled) {
    return;
}

const textPipeline = [{$match: {$text: {$search: "foo"}}}];

const pipeline = [
        {
          $lookup: {
              pipeline: textPipeline,
              from: from.getName(),
              as: "c",
          }
        },
    ];

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(from.insert({_id: 100, a: "foo"}));

// Fail without index.
assertErrorCode(from, textPipeline, ErrorCodes.IndexNotFound);
assertErrorCode(coll, pipeline, ErrorCodes.IndexNotFound);

assert.commandWorked(from.createIndex({a: "text"}));

// Should run when you have the text index.
assert.eq(from.aggregate(textPipeline).itcount(), 1);
assert.eq(coll.aggregate(pipeline).itcount(), 1);
}());
