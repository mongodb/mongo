// Tests that a view dependency chain is correctly logged in the global log for a find command
// against the view.
// @tags: [requires_profiling]

// The entry in the log should contain a field that looks like this:
// "resolvedViews":
// [
//   {
//     "viewNamespace":"<full name of the view>",
//     "dependencyChain":[<names of views/collection in the dep chain, db name omitted>],
//     "resolvedPipeline":[<the resolved pipeline of the view>]
//   },
//   {
//     <repeat for all other views that had to be resolved for the query>
//   }
// ],
(function() {
'use strict';

function resetProfiler(db) {
    assert.commandWorked(db.setProfilingLevel(0, {slowms: 0}));
    db.system.profile.drop();
    assert.commandWorked(db.setProfilingLevel(1, {slowms: 0}));
}

function assertResolvedView(resolvedView, nss, dependencyDepth, pipelineSize) {
    assert.eq(nss, resolvedView["viewNamespace"], resolvedView);
    assert.eq(dependencyDepth, resolvedView["dependencyChain"].length, resolvedView);
    assert.eq(pipelineSize, resolvedView["resolvedPipeline"].length, resolvedView);
}

// Check that the profiler has the expected record.
function checkProfilerLog(db) {
    const result = db.system.profile.find({ns: "views_log_depchain_db.c_view"}).toArray();
    assert.eq(1, result.length, result);
    const record = result[0];
    assert(record.hasOwnProperty("resolvedViews"), record);
    const resolvedViews = record["resolvedViews"];
    assert.eq(2, resolvedViews.length, resolvedViews);

    // The views are logged sorted by their original namespace, not in the order of resolution.
    assertResolvedView(resolvedViews[0], `${db.getName()}.b_view`, 2, 1);
    assertResolvedView(resolvedViews[1], `${db.getName()}.c_view`, 3, 3);
}

const mongodOptions = {};
const conn = MongoRunner.runMongod(mongodOptions);
assert.neq(null, conn, `mongod failed to start with options ${tojson(mongodOptions)}`);

// Setup the tree of views.
const db = conn.getDB(`${jsTest.name()}_db`);
assert.commandWorked(db.dropDatabase());
assert.commandWorked(db.createCollection("base"));
assert.commandWorked(db.createView("a_view", "base", [{$project: {"aa": 0}}, {$sort: {"a": 1}}]));
assert.commandWorked(db.createView("b_view", "base", [{$project: {"bb": 0}}]));
assert.commandWorked(db.createView("c_view", "a_view", [
    {$lookup: {from: "b_view", localField: "x", foreignField: "x", as: "y"}}
]));

// Run a simple query against the top view.
assert.commandWorked(db.setProfilingLevel(1, {slowms: 0}));
assert.commandWorked(db.runCommand({find: "c_view", filter: {}}));
checkProfilerLog(db);

// Sanity-check the "slow query" log (we do it only once, because the data between the profiler
// and "slow query" log are identical).
checkLog.containsWithCount(conn,
                           `"resolvedViews":[{"viewNamespace":"${db.getName()}.b_view",` +
                               `"dependencyChain":["b_view","base"],"resolvedPipeline":[{"$project`,
                           1);

// Run an aggregate query against the top view which uses one of the views from its dependency
// chain, so the logged data is the same as above.
resetProfiler(db);
const lookup = {
    $lookup: {from: "b_view", localField: "x", foreignField: "x", as: "y"}
};
assert.commandWorked(db.runCommand({aggregate: "c_view", pipeline: [lookup], cursor: {}}));
checkProfilerLog(db);

// If a view is modified, the logs should reflect that.
assert.commandWorked(db.runCommand({drop: "c_view"}));
assert.commandWorked(db.createView("c_view", "b_view", [
    {$lookup: {from: "a_view", localField: "x", foreignField: "x", as: "y"}}
]));
resetProfiler(db);
assert.commandWorked(db.runCommand({find: "c_view", filter: {}}));

const result = db.system.profile.find({ns: "views_log_depchain_db.c_view"}).toArray();
assert.eq(1, result.length, result);
const record = result[0];
assert(record.hasOwnProperty("resolvedViews"), record);
const resolvedViews = record["resolvedViews"];
assert.eq(2, resolvedViews.length, resolvedViews);
assertResolvedView(resolvedViews[0], `${db.getName()}.a_view`, 2, 2);
assertResolvedView(resolvedViews[1], `${db.getName()}.c_view`, 3, 2);

assert.commandWorked(db.dropDatabase());
MongoRunner.stopMongod(conn);
})();
