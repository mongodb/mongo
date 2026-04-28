// Tests that $out preserves the collection options of the "out" collection.
// @tags: [
//   does_not_support_transactions,
// ]
import {dropWithoutImplicitRecreate} from "jstests/aggregation/extras/merge_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// TODO (SERVER-124153): Remove the failpoint.
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);
if (!isMultiversion) {
    FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {configureFailPoint: "useInMemoryReplicatedSizeCount", mode: "alwaysOn"},
    });
}

// Setup and populate input collection.
const inputName = "out_preserve_coll_options";
const inputColl = db.out_preserve_coll_options;
inputColl.drop();
assert.commandWorked(inputColl.insert({_id: 0}));

// Setup target collection.
const targetName = inputName + "_target";
dropWithoutImplicitRecreate(targetName);
assert.commandWorked(db.createCollection(targetName, {validationLevel: "moderate"}));

// Verify target collection options.
const targetOptionsResponse = assert.commandWorked(db.runCommand({listCollections: 1, filter: {"name": targetName}}));
const targetOptionsResults = new DBCommandCursor(db, targetOptionsResponse).toArray();
assert.eq(targetOptionsResults.length, 1, targetOptionsResults);
assert.eq({validationLevel: "moderate"}, targetOptionsResults[0].options, targetOptionsResults[0]);

// Run $out pipeline.
inputColl.aggregate([{$out: targetName}]);
const targetColl = db[targetName];
assert.eq(1, targetColl.find().itcount());

// Verify new target collection options.
const targetOptionsResponseNew = assert.commandWorked(
    db.runCommand({listCollections: 1, filter: {"name": targetName}}),
);
const targetOptionsResultsNew = new DBCommandCursor(db, targetOptionsResponseNew).toArray();
assert.eq(targetOptionsResultsNew.length, 1, targetOptionsResultsNew);
assert.eq({validationLevel: "moderate"}, targetOptionsResultsNew[0].options, targetOptionsResultsNew[0]);
