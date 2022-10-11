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

const setup = () => {
    coll.drop();
    stats_coll.drop();
};

const testAnalayzeStats = (key, docs, count) => {
    setup();
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
testAnalayzeStats("a", [{a: 1}], 1);

// Multiple documents single path component.
let docs = [];
for (let i = 1; i < 1001; i++) {
    docs.push({a: i});
}
testAnalayzeStats("a", docs, 1000);

docs = [];
for (let i = 9; ++i < 36;) {
    docs.push({b: i.toString(36).repeat(10)});
}
testAnalayzeStats("b", docs, 26);

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
}());
