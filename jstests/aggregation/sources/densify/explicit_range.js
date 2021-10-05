/**
 * $densify tests with an explicit bounded range without partitions.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

load("jstests/aggregation/sources/densify/libs/densify_in_js.js");

(function() {
"use strict";
const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

// Run all tests for each date unit and on numeric values.
for (let i = 0; i < densifyUnits.length; i++) {
    const unit = densifyUnits[i];
    coll.drop();
    const base = unit ? new ISODate("2021-01-01") : 0;
    const {add} = getArithmeticFunctionsForUnit(unit);

    const runDensifyRangeTest = ({step, bounds}, msg) => testDensifyStage({
        field: "val",
        range: {step, bounds: [add(base, bounds[0]), add(base, bounds[1])], unit: unit}
    },
                                                                          coll,
                                                                          msg);

    // Run all tests for different step values.
    for (let i = 0; i < interestingSteps.length; i++) {
        const step = interestingSteps[i];
        // Generate documents in an empty collection.
        runDensifyRangeTest({step, bounds: [0, 10]});

        // Fill in some documents between existing docs.
        coll.drop();
        coll.insert({val: base});
        coll.insert({val: add(base, 99)});
        runDensifyRangeTest(
            {step, bounds: [10, 25]});  // Checking that the upper bound is exclusive.

        // Fill in odd documents.
        coll.drop();
        insertDocumentsOnStep({base, min: 2, max: 21, step: 2, addFunc: add, coll: coll});
        runDensifyRangeTest({step, bounds: [1, 22]});
        runDensifyRangeTest({step, bounds: [1, 21]});
        runDensifyRangeTest({step, bounds: [1, 20]});

        // Negative numbers.
        coll.drop();
        insertDocumentsOnStep({base, min: -100, max: -1, step: 2, addFunc: add, coll: coll});
        runDensifyRangeTest({step, bounds: [-40, -5]});
        runDensifyRangeTest({step, bounds: [-60, 0]});
        runDensifyRangeTest({step, bounds: [-40, -6]});

        // Extend range past collection.
        coll.drop();
        insertDocumentsOnStep({base, min: 0, max: 50, step: 3, addFunc: add, coll: coll});
        runDensifyRangeTest({step, bounds: [30, 75]});

        // Start range before collection.
        coll.drop();
        insertDocumentsOnStep({base, min: 20, max: 40, step: 2, addFunc: add, coll: coll});
        runDensifyRangeTest({step, bounds: [10, 25]});

        // Extend range in both directions past collection bounds.
        coll.drop();
        insertDocumentsOnStep({base, min: 20, max: 40, step: 2, addFunc: add, coll: coll});
        runDensifyRangeTest({step, bounds: [10, 45]});

        // Different off-step documents.
        coll.drop();
        insertDocumentsOnPredicate(
            {base, min: 0, max: 50, pred: i => i % 3 == 0 || i % 7 == 0, addFunc: add, coll: coll});

        runDensifyRangeTest({step, bounds: [10, 45]});

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
            max: 50,
            pred: i => i % 3 == 0 || i % 7 == 0,
            addFunc: add,
            coll: coll
        });
        runDensifyRangeTest({step, bounds: [10, 45]});
    }
}
})();
