/**
 * Tests that $out cannot be used within a $lookup pipeline.
 *
 * @tags: [requires_fcv_51]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const ERROR_CODE_OUT_BANNED_IN_LOOKUP = 51047;
const coll = db.out_in_lookup_not_allowed;
coll.drop();

const from = db.out_in_lookup_not_allowed_from;
from.drop();

let pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
assertErrorCode(coll, pipeline, ERROR_CODE_OUT_BANNED_IN_LOOKUP);

pipeline = [
        {
          $lookup: {
              pipeline: [{$project: {x: 0}}, {$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];

assertErrorCode(coll, pipeline, ERROR_CODE_OUT_BANNED_IN_LOOKUP);

pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}, {$match: {x: true}}],
              from: from.getName(),
              as: "c",
          }
        },
    ];
assertErrorCode(coll, pipeline, ERROR_CODE_OUT_BANNED_IN_LOOKUP);

// Create view which contains $out within $lookup.
assertDropCollection(coll.getDB(), "view1");

pipeline = [
        {
          $lookup: {
              pipeline: [{$out: "out_collection"}],
              from: from.getName(),
              as: "c",
          }
        },
    ];

// Pipeline will fail because $out is not allowed to exist within a $lookup.
// Validation for $out in a view occurs at a later point.
const cmdRes =
    coll.getDB().runCommand({create: "view1", viewOn: coll.getName(), pipeline: pipeline});
assert.commandFailedWithCode(cmdRes, ERROR_CODE_OUT_BANNED_IN_LOOKUP);
