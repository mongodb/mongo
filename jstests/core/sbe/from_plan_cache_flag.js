// @tags: [
//   # The test runs commands that are not allowed with security token: setProfilingLevel.
//   not_allowed_with_security_token,
//   requires_profiling,
//   does_not_support_stepdowns,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   # TODO SERVER-67607: Test plan cache with CQF enabled.
//   cqf_incompatible,
// ]
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

if (!checkSBEEnabled(db)) {
    jsTest.log("Skip running the test because SBE is not enabled");
    quit();
}
const testDB = db.getSiblingDB("from_plan_cache_flag");
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.getCollection("test");
assert.commandWorked(testDB.setProfilingLevel(2));
coll.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.insert({a: 3}));

const comment = "from_plan_cache_flag";
coll.aggregate([{$match: {a: 1}}], {comment}).toArray();
let profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
assert.eq(!!profileObj.fromPlanCache, false, profileObj);

coll.aggregate([{$match: {a: 2}}], {comment}).toArray();
profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
assert.eq(!!profileObj.fromPlanCache, true, profileObj);

coll.aggregate([{$match: {a: 3}}], {comment}).toArray();
profileObj = getLatestProfilerEntry(testDB, {"command.comment": comment});
assert.eq(!!profileObj.fromPlanCache, true, profileObj);
