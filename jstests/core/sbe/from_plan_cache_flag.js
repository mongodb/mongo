// @tags: [
//   requires_profiling,
//   does_not_support_stepdowns
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.
load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (!checkSBEEnabled(db, ["featureFlagSbeFull"], true /* checkAllNodes */)) {
    jsTest.log("Skip running the test because SBE is not enabled");
    return;
}
var testDB = db.getSiblingDB("profile_findandmodify");
assert.commandWorked(testDB.dropDatabase());
var coll = testDB.getCollection("test");
assert.commandWorked(testDB.setProfilingLevel(2));
coll.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: 3}));
assert.commandWorked(coll.insert({a: 2}));

let pipeline = {$match: {a: 1}};
coll.aggregate([pipeline]).toArray();
let profileObj = getLatestProfilerEntry(testDB);
/* fromPlanCache can be undefined in the profiler entry. The first ! determines the
 * profileObj.fromPlanCache value's associated true/false value (important in the case where
 * undefined) and then returns the opposite of the associated true/false value. The second !
 * returns the opposite of the opposite value. In other words, the !! returns the boolean true/false
 * association of a value.  */
assert.eq(!!profileObj.fromPlanCache, false);

coll.aggregate({$match: {a: 2}}).toArray();
profileObj = getLatestProfilerEntry(testDB);
assert.eq(!!profileObj.fromPlanCache, true);

coll.aggregate({$match: {a: 3}}).toArray();
profileObj = getLatestProfilerEntry(testDB);
assert.eq(!!profileObj.fromPlanCache, true);
}());