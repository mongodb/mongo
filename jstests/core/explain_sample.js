/**
 * Tests for explaining an aggregation pipeline which uses the $sample stage. Only run on WT, since
 * currently only the WT storage engine uses a storage-engine supported random cursor for $sample.
 * @tags: [requires_wiredtiger]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

// Although this test is tagged with 'requires_wiredtiger', this is not sufficient for ensuring
// that the parallel suite runs this test only on WT configurations.
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger") {
    jsTest.log("Skipping test on non-WT storage engine: " + jsTest.options().storageEngine);
    return;
}

const coll = db.explain_sample;
coll.drop();

let docsToInsert = [];
for (let i = 0; i < 1000; ++i) {
    docsToInsert.push({_id: i});
}
assert.commandWorked(coll.insert(docsToInsert));

// Verify that explain reports execution stats for the MULTI_ITERATOR stage. This is designed to
// reproduce SERVER-35973.
const explain =
    assert.commandWorked(coll.explain("allPlansExecution").aggregate([{$sample: {size: 10}}]));
const multiIteratorStages = getAggPlanStages(explain, "MULTI_ITERATOR");
assert.gt(multiIteratorStages.length, 0, tojson(explain));
assert.gt(multiIteratorStages.reduce((acc, stage) => acc + stage.nReturned, 0),
          0,
          tojson(multiIteratorStages));
assert.gt(multiIteratorStages.reduce((acc, stage) => acc + stage.advanced, 0),
          0,
          tojson(multiIteratorStages));
assert.gt(multiIteratorStages.reduce((acc, stage) => acc + stage.works, 0),
          0,
          tojson(multiIteratorStages));
}());
