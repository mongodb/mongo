/**
 * Tests that randomly generated documents can be queried from timeseries collections in the same
 * manner as a tradional collection.
 */
(function() {
"use strict";

load('jstests/third_party/fast_check/fc-3.1.0.js');  // For fast-check (fc).

const scalars = [fc.string(), fc.double(), fc.boolean(), fc.date(), fc.constant(null)];
const pathComponents = fc.constant("a", "b");
// Define our grammar for documents.
let documentModel = fc.letrec(
    tie => ({
        // Our Terminals.
        scalar: fc.oneof(...scalars),
        value: fc.oneof(
            {maxDepth: 3},
            // It may be surprising that we don't have to reweight this. Oneof handles ensuring the
            // termination happens (on sequential runs it weights the first option heavier). The
            // maxDepth parameter also ensures that termination happens by the time the depth is
            // reached.
            // Moreover fast-check prefers simpler object to more complex objects on some runs so we
            // needn't upweight scalars in order to ensure that scalar cases occur more frequently.
            // For more information about the biases fast-check applies see:
            // https://github.com/dubzzz/fast-check/blob/main/packages/fast-check/documentation/HowItWorks.md#bias
            tie('scalar'),
            tie('object'),
            fc.array(tie('value'))),
        object: fc.object({key: pathComponents, maxDepth: 0, maxKeys: 2, values: [tie('value')]}),
    }));

// Define our test arbitraries
const onePath = fc.array(pathComponents, {minLength: 1, maxLength: 2});
const oneComparator = fc.oneof(fc.constant("$lte"), fc.constant("$gte"));
const atLeastThreeDocs = fc.array(documentModel.object, {minLength: 3});

// Our test case
let testMixedTypeQuerying = () => {
    // Assert that query results for ts pushdowns are the same as query results for non-ts
    // collections.
    fc.assert(fc.property(
        // The arbitrary.
        fc.tuple(atLeastThreeDocs, documentModel.scalar, onePath, oneComparator),
        // The scenario to test.
        ([docs, val, pathArray, compare]) => {
            db.test.drop();
            db.control.drop();
            db.createCollection("test", {timeseries: {timeField: "t"}});

            // Insert documents
            docs.forEach(doc => {
                let date = new ISODate();
                db.test.insert(Object.assign({t: date}, doc));
                db.control.insert(Object.assign({t: date}, doc));
            });

            // Construct the path to query on.
            let path = pathArray.join('.');

            // Query on pathArray w/ {[compare]: val} on test and control.
            // Compare the results.
            try {
                assert.docEq(
                    // Isn't timeseries.
                    db.control.find({[path]: {[compare]: val}}, {_id: 0}).toArray(),
                    // Is timeseries.
                    db.test.find({[path]: {[compare]: val}}, {_id: 0}).toArray());
                return true;
            } catch (e) {
                printjson(
                    {info: "test failed", error: e, scenario: [docs, val, pathArray, compare]});
                return false;
            }
        }));
};  // testMixedTypeQuerying

testMixedTypeQuerying();
})();
