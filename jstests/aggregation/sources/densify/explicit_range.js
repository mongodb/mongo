/**
 * $densify tests with an explicit bounded range without partitions.
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
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// On a sharded cluster if the database doesn't exist, densify will return an empty result instead
// of a error.
if (FixtureHelpers.isMongos(db)) {
    // Create database
    assert.commandWorked(db.adminCommand({'enableSharding': db.getName()}));
}

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

// Run all tests for each date unit and on numeric values.
for (let i = 0; i < densifyUnits.length; i++) {
    coll.drop();

    const unit = densifyUnits[i];
    const base = unit ? new ISODate("2021-01-01") : 0;
    const {add} = getArithmeticFunctionsForUnit(unit);

    const getBounds = (lower, upper) => {
        return [add(base, lower), add(base, upper)];
    };

    // Run all tests for different step values.
    for (let i = 0; i < interestingSteps.length; i++) {
        const step = interestingSteps[i];

        // Generate documents in an empty collection.
        let stage = {field: "val", range: {step: step, bounds: getBounds(0, 10), unit: unit}};
        testDensifyStage(stage, coll);

        // Fill in some documents between existing docs.
        coll.drop();
        coll.insert({val: base});
        coll.insert({val: add(base, 30)});
        // Checking that the upper bound is exclusive.
        stage = {field: "val", range: {step: step, bounds: getBounds(10, 25), unit: unit}};
        testDensifyStage(stage, coll);

        // Fill in odd documents.
        coll.drop();
        insertDocumentsOnStep({base, min: 2, max: 11, step: 2, addFunc: add, coll: coll});

        stage = {field: "val", range: {step: step, bounds: getBounds(1, 12), unit: unit}};
        testDensifyStage(stage, coll);
        stage = {field: "val", range: {step: step, bounds: getBounds(1, 11), unit: unit}};
        testDensifyStage(stage, coll);
        stage = {field: "val", range: {step: step, bounds: getBounds(1, 10), unit: unit}};
        testDensifyStage(stage, coll);

        // Negative numbers.
        coll.drop();
        insertDocumentsOnStep({base, min: -20, max: -1, step: 2, addFunc: add, coll: coll});
        stage = {field: "val", range: {step: step, bounds: getBounds(-10, -1), unit: unit}};
        testDensifyStage(stage, coll);
        stage = {field: "val", range: {step: step, bounds: getBounds(-10, 0), unit: unit}};
        testDensifyStage(stage, coll);
        stage = {field: "val", range: {step: step, bounds: getBounds(-10, -2), unit: unit}};
        testDensifyStage(stage, coll);

        // Extend range past collection.
        coll.drop();
        insertDocumentsOnStep({base, min: 0, max: 10, step: 3, addFunc: add, coll: coll});
        stage = {field: "val", range: {step: step, bounds: getBounds(5, 15), unit: unit}};
        testDensifyStage(stage, coll);

        // Start range before collection.
        coll.drop();
        insertDocumentsOnStep({base, min: 20, max: 30, step: 2, addFunc: add, coll: coll});
        stage = {field: "val", range: {step: step, bounds: getBounds(10, 25), unit: unit}};
        testDensifyStage(stage, coll);

        // Extend range in both directions past collection bounds.
        coll.drop();
        insertDocumentsOnStep({base, min: 20, max: 30, step: 2, addFunc: add, coll: coll});
        stage = {field: "val", range: {step: step, bounds: getBounds(10, 35), unit: unit}};
        testDensifyStage(stage, coll);

        // Different off-step documents.
        coll.drop();
        insertDocumentsOnPredicate(
            {base, min: 0, max: 25, pred: i => i % 3 == 0 || i % 7 == 0, addFunc: add, coll: coll});
        stage = {field: "val", range: {step: step, bounds: getBounds(10, 20), unit: unit}};
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
            max: 30,
            pred: i => i % 3 == 0 || i % 7 == 0,
            addFunc: add,
            coll: coll
        });
        stage = {field: "val", range: {step: step, bounds: getBounds(10, 25), unit: unit}};
        testDensifyStage(stage, coll);
    }
}

// Run a test where there are no documents in the range to ensure we don't generate anything before
// the range.
coll.drop();
let documents = [
    {"date": ISODate("2022-10-29T23:00:00Z")},
];
coll.insert(documents);
let stage = {
    field: "date",
    range: {
        step: 1,
        unit: "month",
        bounds: [
            ISODate("2022-10-31T23:00:00.000Z"),
            ISODate("2022-11-30T23:00:00.000Z"),
        ],
    },
};
testDensifyStage(stage, coll, "Ensure no docs before range");
