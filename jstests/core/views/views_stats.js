// Test that top and latency histogram statistics are recorded for views.
//
// This test attempts to perform write operations and get latency statistics using the $collStats
// stage. The former operation must be routed to the primary in a replica set, whereas the latter
// may be routed to a secondary.
//
// @tags: [
//     # The test runs commands that are not allowed with security token: top.
//     not_allowed_with_signed_security_token,
//     assumes_read_preference_unchanged,
//     assumes_unsharded_collection,
//     does_not_support_repeated_reads,
// ]

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertHistogramDiffEq,
    assertTopDiffEq,
    getHistogramStats,
    getTop
} from "jstests/libs/stats.js";

let viewsDB = db.getSiblingDB("views_stats");
assert.commandWorked(viewsDB.dropDatabase());
assert.commandWorked(viewsDB.runCommand({create: "view", viewOn: "collection"}));

let view = viewsDB["view"];
let coll = viewsDB["collection"];

assert.commandWorked(coll.insert({val: 'TestValue'}));

// Check the histogram counters.
let lastHistogram = getHistogramStats(view);
view.aggregate([{$match: {}}]);
lastHistogram = assertHistogramDiffEq(viewsDB, view, lastHistogram, 1, 0, 0);

// Check that failed inserts, updates, and deletes are counted.
assert.writeError(view.insert({}));
lastHistogram = assertHistogramDiffEq(viewsDB, view, lastHistogram, 0, 1, 0);

assert.writeError(view.remove({}));
lastHistogram = assertHistogramDiffEq(viewsDB, view, lastHistogram, 0, 1, 0);

assert.writeError(view.update({}, {}));
lastHistogram = assertHistogramDiffEq(viewsDB, view, lastHistogram, 0, 1, 0);

if (FixtureHelpers.isMongos(viewsDB)) {
    jsTest.log("Tests are being run on a mongos; skipping top tests.");
    quit();
}

// Check the top counters.
let lastTop = getTop(view);
if (lastTop === undefined) {
    quit();
}

view.aggregate([{$match: {}}]);
lastTop = assertTopDiffEq(viewsDB, view, lastTop, "commands", 1);

assert.writeError(view.insert({}));
lastTop = assertTopDiffEq(viewsDB, view, lastTop, "insert", 1);

assert.writeError(view.remove({}));
lastTop = assertTopDiffEq(viewsDB, view, lastTop, "remove", 1);

assert.writeError(view.update({}, {}));
lastTop = assertTopDiffEq(viewsDB, view, lastTop, "update", 1);

// Check that operations on the backing collection do not modify the view stats.
lastTop = getTop(view);
if (lastTop === undefined) {
    quit();
}

lastHistogram = getHistogramStats(view);
assert.commandWorked(coll.insert({}));
assert.commandWorked(coll.update({}, {$set: {x: 1}}));
coll.aggregate([{$match: {}}]);
assert.commandWorked(coll.remove({}));

assertTopDiffEq(viewsDB, view, lastTop, "insert", 0);
assertTopDiffEq(viewsDB, view, lastTop, "update", 0);
assertTopDiffEq(viewsDB, view, lastTop, "remove", 0);
assertHistogramDiffEq(viewsDB, view, lastHistogram, 0, 0, 0);
