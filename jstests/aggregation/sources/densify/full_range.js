/**
 * $densify tests with full range without partitions.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

import {
    densifyUnits,
    getArithmeticFunctionsForUnit,
    insertDocumentsOnPredicate,
    insertDocumentsOnStep,
    interestingSteps,
    testDensifyStage,
} from "jstests/aggregation/sources/densify/libs/densify_in_js.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

// Run all tests for each date unit and on numeric values.
for (let i = 0; i < densifyUnits.length; i++) {
    coll.drop();

    const unit = densifyUnits[i];
    const base = unit ? new ISODate("2021-01-01") : 0;
    const {add} = getArithmeticFunctionsForUnit(unit);

    // Run all tests for different step values.
    for (let i = 0; i < interestingSteps.length; i++) {
        const step = interestingSteps[i];
        const stage = {field: "val", range: {step: step, bounds: "full", unit: unit}};

        // Fill in docs between 1 and 10.
        coll.drop();
        coll.insert({val: base});
        coll.insert({val: add(base, 10)});
        testDensifyStage(stage, coll);

        // Negative numbers and dates before the epoch.
        coll.drop();
        insertDocumentsOnStep({base, min: -10, max: -1, step: 2, addFunc: add, coll: coll});
        testDensifyStage(stage, coll);

        // Lots of off-step documents.
        coll.drop();
        insertDocumentsOnPredicate(
            {base, min: 0, max: 10, pred: i => i % 3 == 0 || i % 7 == 0, addFunc: add, coll: coll});
        testDensifyStage(stage, coll);

        // Lots of off-step documents with nulls sprinkled in to confirm that a null value is
        // treated the same as a missing value.
        coll.drop();
        insertDocumentsOnPredicate(
            {base, min: 0, max: 10, pred: i => i % 3 == 0 || i % 7 == 0, addFunc: add, coll: coll});
        coll.insert({val: null});
        insertDocumentsOnPredicate({
            base,
            min: 10,
            max: 20,
            pred: i => i % 3 == 0 || i % 7 == 0,
            addFunc: add,
            coll: coll
        });
        coll.insert({val: null});
        coll.insert({blah: base});  // Missing "val" key.
        insertDocumentsOnPredicate({
            base,
            min: 20,
            max: 25,
            pred: i => i % 3 == 0 || i % 7 == 0,
            addFunc: add,
            coll: coll
        });

        testDensifyStage(stage, coll);
    }
}

// Test that full range does not fail if there's only one document in the collection.
coll.drop();
coll.insert({_id: 1, val: 1, orig: true});
let result = coll.aggregate([
    {"$densify": {"field": "val", "range": {"step": 2, "bounds": "full"}}},
]);
const expected = [{_id: 1, val: 1, orig: true}];
const resultArray = result.toArray();
assert.sameMembers(resultArray, expected);
