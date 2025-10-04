// Test the count command with views.
//
// @tags: [
//   assumes_unsharded_collection,
//   requires_fastcount,
//   # Explain of a resolved view must be executed by mongos.
//   directly_against_shardsvrs_incompatible,
//   requires_fcv_63,
// ]

import {getEngine, getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

const viewsDB = db.getSiblingDB("views_count");
assert.commandWorked(viewsDB.dropDatabase());

// Insert documents into a collection.
let coll = viewsDB.getCollection("coll");
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 10; i++) {
    bulk.insert({x: i});
}
assert.commandWorked(bulk.execute());

// Create views on the data.
assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: "coll"}));
assert.commandWorked(
    viewsDB.runCommand({create: "greaterThanThreeView", viewOn: "coll", pipeline: [{$match: {x: {$gt: 3}}}]}),
);
assert.commandWorked(
    viewsDB.runCommand({
        create: "lessThanSevenView",
        viewOn: "greaterThanThreeView",
        pipeline: [{$match: {x: {$lt: 7}}}],
    }),
);
let identityView = viewsDB.getCollection("identityView");
let greaterThanThreeView = viewsDB.getCollection("greaterThanThreeView");
let lessThanSevenView = viewsDB.getCollection("lessThanSevenView");

// Count on a view, with or without a query.
assert.eq(coll.count(), identityView.count());
assert.eq(coll.count({}), identityView.count({}));
assert.eq(coll.count({x: {$exists: true}}), identityView.count({x: {$exists: true}}));
assert.eq(coll.count({x: 0}), identityView.count({x: 0}));
assert.eq(6, greaterThanThreeView.count());
assert.eq(6, greaterThanThreeView.count({}));
assert.eq(3, lessThanSevenView.count());
assert.eq(3, lessThanSevenView.count({}));

// Test empty counts.
assert.eq(coll.count({x: -1}), identityView.count({x: -1}));
assert.eq(0, greaterThanThreeView.count({x: 2}));
assert.eq(0, lessThanSevenView.count({x: 9}));

// Counting on views works with limit and skip.
assert.eq(7, identityView.count({x: {$exists: true}}, {skip: 3}));
assert.eq(3, greaterThanThreeView.count({x: {$lt: 100}}, {limit: 3}));
assert.eq(1, lessThanSevenView.count({}, {skip: 1, limit: 1}));

// Count with explain works on a view.
assert.commandWorked(lessThanSevenView.explain().count());
assert.commandWorked(greaterThanThreeView.explain().count({x: 6}));
let explainPlan = lessThanSevenView.explain().count({foo: "bar"});
assert.commandWorked(explainPlan);
explainPlan = getSingleNodeExplain(explainPlan);
if (explainPlan.hasOwnProperty("stages")) {
    explainPlan = explainPlan.stages[0].$cursor;
}
assert.eq(explainPlan.queryPlanner.namespace, "views_count.coll");

// Count with explicit explain modes works on a view.
explainPlan = assert.commandWorked(lessThanSevenView.explain("queryPlanner").count({x: {$gte: 5}}));
explainPlan = getSingleNodeExplain(explainPlan);
if (explainPlan.hasOwnProperty("stages")) {
    explainPlan = explainPlan.stages[0].$cursor;
}
assert.eq(explainPlan.queryPlanner.namespace, "views_count.coll");
assert(!explainPlan.hasOwnProperty("executionStats"));

explainPlan = assert.commandWorked(lessThanSevenView.explain("executionStats").count({x: {$gte: 5}}));
explainPlan = getSingleNodeExplain(explainPlan);
if (explainPlan.hasOwnProperty("stages")) {
    explainPlan = explainPlan.stages[0].$cursor;
}
assert.eq(explainPlan.queryPlanner.namespace, "views_count.coll");
assert(explainPlan.hasOwnProperty("executionStats"));
assert.eq(
    explainPlan.executionStats.nReturned,
    getEngine(explainPlan) === "sbe" ? 1 : 2, // SBE group push down causes count to be emitted in first stage.
    tojson(explainPlan),
);
assert(!explainPlan.executionStats.hasOwnProperty("allPlansExecution"));

explainPlan = assert.commandWorked(lessThanSevenView.explain("allPlansExecution").count({x: {$gte: 5}}));
explainPlan = getSingleNodeExplain(explainPlan);
if (explainPlan.hasOwnProperty("stages")) {
    explainPlan = explainPlan.stages[0].$cursor;
}
assert.eq(explainPlan.queryPlanner.namespace, "views_count.coll");
assert(explainPlan.hasOwnProperty("executionStats"));
assert.eq(
    explainPlan.executionStats.nReturned,
    getEngine(explainPlan) === "sbe" ? 1 : 2, // SBE group push down causes count to be emitted in first stage.
    tojson(explainPlan),
);
assert(explainPlan.executionStats.hasOwnProperty("allPlansExecution"));

// Count with hint works on a view.
assert.commandWorked(viewsDB.runCommand({count: "identityView", hint: "_id_"}));

assert.commandFailedWithCode(
    viewsDB.runCommand({count: "identityView", collation: {locale: "en_US"}}),
    ErrorCodes.OptionNotSupportedOnView,
);
