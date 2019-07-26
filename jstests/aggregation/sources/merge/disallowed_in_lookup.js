// Tests that $merge cannot be used within a $lookup pipeline.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");                     // For assertErrorCode.
load("jstests/libs/collection_drop_recreate.js");                // For assertDropCollection.
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findNonConfigNodes.
load("jstests/libs/fixture_helpers.js");                         // For isSharded.

const kErrorCodeMergeBannedInLookup = 51047;
const kErrorCodeMergeLastStageOnly = 40601;
const coll = db.merge_in_lookup_not_allowed;
coll.drop();

const from = db.merge_in_lookup_not_allowed_from;
from.drop();

if (FixtureHelpers.isSharded(from)) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                           "internalQueryAllowShardedLookup",
                           true);
}

let pipeline = [
        {
          $lookup: {
              pipeline: [{$merge: {into: "out_collection", on: "_id"}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
assertErrorCode(coll, pipeline, kErrorCodeMergeBannedInLookup);

pipeline = [
        {
          $lookup: {
              pipeline: [{$project: {x: 0}}, {$merge: {into: "out_collection", on: "_id"}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
assertErrorCode(coll, pipeline, kErrorCodeMergeBannedInLookup);

pipeline = [
        {
          $lookup: {
              pipeline: [{$merge: {into: "out_collection", on: "_id"}}, {$match: {x: true}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
// Pipeline will fail because $merge is not last in the subpipeline.
// Validation for $merge in a $lookup's subpipeline occurs at a later point.
assertErrorCode(coll, pipeline, kErrorCodeMergeLastStageOnly);

// Create view which contains $merge within $lookup.
assertDropCollection(coll.getDB(), "view1");

pipeline = [
        {
          $lookup: {
              pipeline: [{$merge: {into: "out_collection", on: "_id"}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
// Pipeline will fail because $merge is not allowed to exist within a $lookup.
// Validation for $merge in a view occurs at a later point.
const cmdRes =
    coll.getDB().runCommand({create: "view1", viewOn: coll.getName(), pipeline: pipeline});
assert.commandFailedWithCode(cmdRes, kErrorCodeMergeBannedInLookup);

// Test that a $merge without an explicit "on" field still fails within a $lookup.
pipeline = [
        {
          $lookup: {
              pipeline: [{$merge: {into: "out_collection"}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
    kErrorCodeMergeBannedInLookup);
}());
