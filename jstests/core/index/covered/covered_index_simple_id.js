// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]
// Simple covered index query test

// Include helpers for analyzing explain output.
import {getOptimizer, isIndexOnly} from "jstests/libs/analyze_plan.js";

var coll = db.getCollection("covered_simple_id");
coll.drop();
for (let i = 0; i < 10; i++) {
    coll.insert({_id: i});
}
coll.insert({_id: "string"});
coll.insert({_id: {bar: 1}});
coll.insert({_id: null});

// Test equality with int value
var plan = coll.find({_id: 1}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
       "simple.id.1 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.1 - docs examined should be 0 for covered query");

// Test equality with string value
var plan = coll.find({_id: "string"}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
       "simple.id.2 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.2 - docs examined should be 0 for covered query");

// Test equality with int value on a dotted field
var plan = coll.find({_id: {bar: 1}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
       "simple.id.3 - indexOnly should be true on covered query");
assert.eq(0,
          plan.executionStats.totalDocsExamined,
          "simple.id.3 - docs examined should be 0 for covered query");

// Test no query
if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on the cursor_hints_to_query_settings
    // suite. The suite replaces cursor hints with query settings. Query settings do not force
    // indexes, and therefore empty filter will result in collection scans.
    var plan = coll.find({}, {_id: 1}).hint({_id: 1}).explain("executionStats");
    assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
           "simple.id.4 - indexOnly should be true on covered query");
    assert.eq(0,
              plan.executionStats.totalDocsExamined,
              "simple.id.4 - docs examined should be 0 for covered query");
}

// Test range query
var plan = coll.find({_id: {$gt: 2, $lt: 6}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
switch (getOptimizer(plan)) {
    case "classic":
        assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
               "simple.id.5 - indexOnly should be true on covered query");
        assert.eq(0,
                  plan.executionStats.totalDocsExamined,
                  "simple.id.5 - docs examined should be 0 for covered query");
        break;
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
    default:
        break
}

// Test in query
var plan = coll.find({_id: {$in: [5, 8]}}, {_id: 1}).hint({_id: 1}).explain("executionStats");
switch (getOptimizer(plan)) {
    case "classic":
        assert(isIndexOnly(db, plan.queryPlanner.winningPlan),
               "simple.id.6 - indexOnly should be true on covered query");
        assert.eq(0,
                  plan.executionStats.totalDocsExamined,
                  "simple.id.6 - docs examined should be 0 for covered query");
        break;
    case "CQF":
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
    default:
        break
}
print('all tests pass');
