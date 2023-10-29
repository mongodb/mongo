// Tests that $out can only be used as the last stage.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// TODO SERVER-82460 remove creation of database once
// $out behavior will be equal in both standalone and sharded cluster
if (FixtureHelpers.isMongos(db)) {
    // Create database
    assert.commandWorked(db.adminCommand({'enableSharding': db.getName()}));
}

const coll = db.require_out_last;
coll.drop();

// Test that $out is allowed as the last (and only) stage.
assert.doesNotThrow(() => coll.aggregate([{$out: "out_collection"}]));

// Test that $out is not allowed to have a stage after it.
assertErrorCode(coll, [{$out: "out_collection"}, {$match: {x: true}}], 40601);
assertErrorCode(coll, [{$project: {x: 0}}, {$out: "out_collection"}, {$match: {x: true}}], 40601);