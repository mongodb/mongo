/**
 * Testing of just the query layer's integration for columnar index.
 * This test is intended to be temporary.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db);

if (!isSBEEnabled) {
    // This test is only relevant when SBE is enabled.
    return;
}

const testDB = db;
const coll = db.column_index_skeleton;
coll.drop();

assert.commandWorked(coll.insert({a: [[1, 2], [{b: [[1, 2], [{c: [[1, 2], [{}], 2]}], 2]}], 2]}));

// Enable the columnar fail point.
const failPoint = configureFailPoint(testDB, "includeFakeColumnarIndex");
try {
    // Run an explain.
    const expl = coll.find({}, {a: 1}).explain();
    assert(planHasStage(db, expl, "COLUMN_IXSCAN"));

    // Run a query.
    let results = coll.find({}, {_id: 0, "a.b.c": 1}).toArray();
    assert.eq(results.length, 1);
    assert.eq(results[0], {a: [[], [{b: [[], [{c: [[1, 2], [{}], 2]}]]}]]});
} finally {
    failPoint.off();
}
})();
