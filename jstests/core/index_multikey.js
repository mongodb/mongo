/**
 * Tests that an index properly reports multikeyness and multikey paths metadata in the presence of
 * successful and unsuccessful inserts.
 */
(function() {
"use strict";

// For making assertions about explain output.
load("jstests/libs/analyze_plan.js");

const coll = db.getCollection("index_multikey");
coll.drop();

function getIndexScanExplainOutput() {
    const explain = coll.find().hint({a: 1, b: 1}).explain();
    assert.commandWorked(explain);
    return getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN");
}

assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({"a.0.0": 1}));
let ixscan = getIndexScanExplainOutput();
assert.eq(
    ixscan.isMultiKey, false, "empty index should not be marked multikey; plan: " + tojson(ixscan));
assert.eq(ixscan.multiKeyPaths,
          {a: [], b: []},
          "empty index should have no multiKeyPaths; plan: " + tojson(ixscan));

assert.commandWorked(coll.insert({a: [1, 2, 3]}));
ixscan = getIndexScanExplainOutput();
assert.eq(ixscan.isMultiKey,
          true,
          "index should have been marked as multikey after insert; plan: " + tojson(ixscan));
assert.eq(ixscan.multiKeyPaths,
          {a: ["a"], b: []},
          "index has wrong multikey paths after insert; plan: " + ixscan);
})();
