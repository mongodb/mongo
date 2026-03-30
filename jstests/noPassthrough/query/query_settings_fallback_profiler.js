// Tests that the multiPlannerFallbackEngaged flag is correctly set in the profiler when
// query settings fail to produce a valid plan and the planner falls back to multi-planning.
// @tags: [
//   requires_profiling,
// ]

import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
const coll = db.getCollection("test");
coll.drop();

// Enable profiling.
assert.commandWorked(db.setProfilingLevel(2));
const profileEntryFilter = {op: "query", "command.find": "test"};

// Setup: create two indexes and insert data.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (let i = 0; i < 5; ++i) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

const qsutils = new QuerySettingsUtils(db, coll.getName());
const ns = {db: db.getName(), coll: coll.getName()};

//
// Confirm multiPlannerFallbackEngaged is true when query settings fallback is engaged.
//
const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}});
const settings = {indexHints: {ns, allowedIndexes: ["doesnotexist"]}};

qsutils.withQuerySettings(querySettingsQuery, settings, () => {
    // Clear plan cache and profiler to ensure clean state.
    coll.getPlanCache().clear();
    assert.commandWorked(db.setProfilingLevel(0));
    db.system.profile.drop();
    assert.commandWorked(db.setProfilingLevel(2));

    // Use find().toArray() instead of findOne() to avoid express fast path.
    const results = coll.find({a: 1, b: 1}).toArray();
    assert.gt(results.length, 0);
    const profileObj = getLatestProfilerEntry(db, profileEntryFilter);
    assert.eq(profileObj.multiPlannerFallbackEngaged, true, profileObj);
});

//
// Confirm multiPlannerFallbackEngaged is absent when no query settings are present.
//
coll.getPlanCache().clear();
assert.commandWorked(db.setProfilingLevel(0));
db.system.profile.drop();
assert.commandWorked(db.setProfilingLevel(2));

const results = coll.find({a: 1, b: 1}).toArray();
assert.gt(results.length, 0);
let profileObj = getLatestProfilerEntry(db, profileEntryFilter);
assert.eq(profileObj.multiPlannerFallbackEngaged, undefined, profileObj);

rst.stopSet();
