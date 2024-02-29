/**
 * Test that a query with a filter containing a $where expression gets successfully replanned.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

const conn = MongoRunner.runMongod({});
const db = conn.getDB("test");

// TODO SERVER-85239: Remove this check when classic replanning for agg pipeline is implemented.
if (FeatureFlagUtil.isPresentAndEnabled(db, "ClassicRuntimePlanningForSbe")) {
    jsTestLog("Skipping test since featureFlagClassicRuntimePlanningForSbe is enabled");
    MongoRunner.stopMongod(conn);
    quit();
}

const coll = db.plan_cache_replan_where;
coll.drop();

// Create two indexes to ensure the plan gets cached when the classic engine is used.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
const docs = Array.from({length: 20}, (_, i) => ({a: 1, b: i}));
assert.commandWorked(coll.insert(docs));
// Insert an extra document such that the initial query has a single document to return.
assert.commandWorked(coll.insert({a: 2, y: 1}));

assert.eq(1,
          coll.find({
                  $and: [
                      {a: 2},
                      {
                          $where: function() {
                              return true
                          }
                      }
                  ]
              })
              .itcount());
// We need to run the query twice for it to be marked "active" in the plan cache.
assert.eq(1,
          coll.find({
                  $and: [
                      {a: 2},
                      {
                          $where: function() {
                              return true
                          }
                      }
                  ]
              })
              .itcount());

const cachedPlans = coll.getPlanCache().list();
assert.eq(1, cachedPlans.length, cachedPlans);
assert.eq(true, cachedPlans[0].isActive, cachedPlans);

// Assert we "replan", by running the same query with different parameters. This time the filter is
// not selective at all and will result in more documents being filtered out.
assert.commandWorked(db.setProfilingLevel(2));
assert.eq(20,
          coll.find({
                  $and: [
                      {a: 1},
                      {
                          $where: function() {
                              return true
                          }
                      }
                  ]
              })
              .itcount());

const profileObj = getLatestProfilerEntry(db, {op: "query"});
assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.replanned, true, profileObj);

MongoRunner.stopMongod(conn);
