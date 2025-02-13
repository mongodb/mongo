/**
 * Tests that $merge cannot be used within a $lookup pipeline.
 *
 * @tags: [requires_fcv_51]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const kErrorCodeMergeBannedInLookup = 51047;
const coll = db.merge_in_lookup_not_allowed;
coll.drop();

const from = db.merge_in_lookup_not_allowed_from;
from.drop();

// TODO SERVER-86712 remove creation of database once
// $lookup/$merge behavior will be equal in both standalone and sharded cluster
if (FixtureHelpers.isMongos(db)) {
    // Create database
    assert.commandWorked(db.adminCommand({'enableSharding': db.getName()}));
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
assertErrorCode(coll, pipeline, kErrorCodeMergeBannedInLookup);

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
