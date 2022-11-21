(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

if (checkSBEEnabled(db, ["featureFlagSbeFull"], true)) {
    jsTestLog("Skipping the test because it doesn't work in Full SBE");
    return;
}

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "tryBonsai"}));

const coll = db.cqf_analyze_scalar_hist;
const stats_coll = db.system.statistics.cqf_analyze_scalar_hist;

const testAnalyzeStats = (key, docs, count) => {
    // Populate with documents.
    coll.insertMany(docs);

    // Check the stats collection is created, data is inserted, and the index is created.
    let res = db.runCommand({analyze: coll.getName(), key: key});
    assert.commandWorked(res);
    stats_coll.find().forEach(function(doc) {
        jsTestLog(doc);
    });
    let stats = stats_coll.find({_id: key})[0];
    assert.eq(stats["statistics"]["documents"], count);
};

// Single document single path component.
testAnalyzeStats("a", [{a: 1}], 1);
let stats_doc = stats_coll.findOne({_id: "a"});
assert.eq(stats_doc.statistics.scalarHistogram.bounds[0], 1);

// Reanalyze the same path.
coll.drop();
testAnalyzeStats("a", [{a: 2}], 1);
stats_doc = stats_coll.findOne({_id: "a"});
assert.eq(stats_doc.statistics.scalarHistogram.bounds[0], 2);

// Multiple documents single path component. Note that we haven't dropped the document {a: 2}, so it
// will be included in our document count with field "b" considered as a missing value (10 + 1).
let docs = [];
for (let i = 1; i < 11; i++) {
    docs.push({b: i});
}
testAnalyzeStats("b", docs, 11);

docs = [];
for (let i = 9; ++i < 36;) {
    docs.push({c: i.toString(36).repeat(10)});
}
// Once again, we still have the documents with only paths "a" & "b" (no "c"), so the histogram
// document count should accurately reflect this (26 + 11).
testAnalyzeStats("c", docs, 37);

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
}());
