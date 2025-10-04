// Tests that $out can only be used as the last stage.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.require_out_last;
coll.drop();

// Test that $out is allowed as the last (and only) stage.
assert.doesNotThrow(() => coll.aggregate([{$out: "out_collection"}]));

// Test that $out is not allowed to have a stage after it.
assertErrorCode(coll, [{$out: "out_collection"}, {$match: {x: true}}], 40601);
assertErrorCode(coll, [{$project: {x: 0}}, {$out: "out_collection"}, {$match: {x: true}}], 40601);
