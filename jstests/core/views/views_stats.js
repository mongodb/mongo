// Test that top and latency histogram statistics are recorded for views.
//
// This test attempts to perform write operations and get latency statistics using the $collStats
// stage. The former operation must be routed to the primary in a replica set, whereas the latter
// may be routed to a secondary.
//
// The test runs commands that are not allowed with security token: top.
// @tags: [
//     not_allowed_with_security_token,
//     assumes_read_preference_unchanged,
//     assumes_unsharded_collection,
//     # This test depends on hardcoded database name equality.
//     tenant_migration_incompatible,
//     does_not_support_repeated_reads,
// ]

(function() {
"use strict";
load("jstests/libs/stats.js");
load("jstests/libs/fixture_helpers.js");

let viewsDB = db.getSiblingDB("views_stats");
assert.commandWorked(viewsDB.dropDatabase());
assert.commandWorked(viewsDB.runCommand({create: "view", viewOn: "collection"}));

let view = viewsDB["view"];
let coll = viewsDB["collection"];

assert.commandWorked(coll.insert({val: 'TestValue'}));

// Check the histogram counters.
let lastHistogram = getHistogramStats(view);
view.aggregate([{$match: {}}]);
lastHistogram = assertHistogramDiffEq(view, lastHistogram, 1, 0, 0);

// Check that failed inserts, updates, and deletes are counted.
assert.writeError(view.insert({}));
lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

assert.writeError(view.remove({}));
lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

assert.writeError(view.update({}, {}));
lastHistogram = assertHistogramDiffEq(view, lastHistogram, 0, 1, 0);

if (FixtureHelpers.isMongos(viewsDB)) {
    jsTest.log("Tests are being run on a mongos; skipping top tests.");
    return;
}

// Check the top counters.
let lastTop = getTop(view);
if (lastTop === undefined) {
    return;
}

view.aggregate([{$match: {}}]);
lastTop = assertTopDiffEq(view, lastTop, "commands", 1);

assert.writeError(view.insert({}));
lastTop = assertTopDiffEq(view, lastTop, "insert", 1);

assert.writeError(view.remove({}));
lastTop = assertTopDiffEq(view, lastTop, "remove", 1);

assert.writeError(view.update({}, {}));
lastTop = assertTopDiffEq(view, lastTop, "update", 1);

// Check that operations on the backing collection do not modify the view stats.
lastTop = getTop(view);
if (lastTop === undefined) {
    return;
}

lastHistogram = getHistogramStats(view);
assert.commandWorked(coll.insert({}));
assert.commandWorked(coll.update({}, {$set: {x: 1}}));
coll.aggregate([{$match: {}}]);
assert.commandWorked(coll.remove({}));

assertTopDiffEq(view, lastTop, "insert", 0);
assertTopDiffEq(view, lastTop, "update", 0);
assertTopDiffEq(view, lastTop, "remove", 0);
assertHistogramDiffEq(view, lastHistogram, 0, 0, 0);
}());
