/**
 * Tests the find command on views.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_getmore,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   requires_fcv_73,
 * ]
 */
import {arrayEq, orderedArrayEq} from "jstests/aggregation/extras/utils.js";
import {getSingleNodeExplain} from "jstests/libs/analyze_plan.js";

let viewsDB = db.getSiblingDB("views_find");
assert.commandWorked(viewsDB.dropDatabase());

// Helper functions.
let assertFindResultEq = function(cmd, expected, ordered) {
    let res = viewsDB.runCommand(cmd);
    assert.commandWorked(res);
    let arr = new DBCommandCursor(viewsDB, res, 5).toArray();
    let errmsg = tojson({expected: expected, got: arr});

    if (typeof (ordered) === "undefined" || !ordered)
        assert(arrayEq(arr, expected), errmsg);
    else
        assert(orderedArrayEq(arr, expected), errmsg);
};

// Populate a collection with some test data.
let allDocuments = [];
allDocuments.push({_id: "New York", state: "NY", pop: 7});
allDocuments.push({_id: "Newark", state: "NJ", pop: 3});
allDocuments.push({_id: "Palo Alto", state: "CA", pop: 10});
allDocuments.push({_id: "San Francisco", state: "CA", pop: 4});
allDocuments.push({_id: "Trenton", state: "NJ", pop: 5});

let coll = viewsDB.coll;
let bulk = coll.initializeUnorderedBulkOp();
allDocuments.forEach(function(doc) {
    bulk.insert(doc);
});
assert.commandWorked(bulk.execute());

// Create views on the data.
assert.commandWorked(
    viewsDB.runCommand({create: "identityView", viewOn: "coll", pipeline: [{$match: {}}]}));
assert.commandWorked(viewsDB.runCommand({
    create: "noIdView",
    viewOn: "coll",
    pipeline: [{$match: {}}, {$project: {_id: 0, state: 1, pop: 1}}]
}));

// Filters and "simple" projections.
assertFindResultEq({find: "identityView"}, allDocuments);
assertFindResultEq({find: "identityView", filter: {state: "NJ"}, projection: {_id: 1}},
                   [{_id: "Trenton"}, {_id: "Newark"}]);

// A view that projects out the _id should still work with the find command.
assertFindResultEq({find: "noIdView", filter: {state: "NY"}, projection: {pop: 1}}, [{pop: 7}]);

// Sort, limit and batchSize.
const doOrderedSort = true;
assertFindResultEq({find: "identityView", sort: {_id: 1}}, allDocuments, doOrderedSort);
assertFindResultEq(
    {find: "identityView", limit: 1, batchSize: 1, sort: {_id: 1}, projection: {_id: 1}},
    [{_id: "New York"}]);

// $natural sort against a view is permitted, since it has the same meaning as $natural hint.
// Likewise, $natural hint against a view is permitted.
assertFindResultEq({find: "identityView", filter: {state: "NY"}, sort: {$natural: 1}},
                   [{_id: "New York", state: "NY", pop: 7}]);
assertFindResultEq({find: "identityView", filter: {state: "NY"}, hint: {$natural: 1}},
                   [{_id: "New York", state: "NY", pop: 7}]);

// Negative batch size and limit should fail.
assert.commandFailed(viewsDB.runCommand({find: "identityView", batchSize: -1}));
assert.commandFailed(viewsDB.runCommand({find: "identityView", limit: -1}));

// Comment should succeed.
assert.commandWorked(viewsDB.runCommand({find: "identityView", filter: {}, comment: "views_find"}));

// Views support find with explain.
assert.commandWorked(viewsDB.identityView.find().explain());

// Find with explicit explain modes works on a view.
let explainPlan = assert.commandWorked(viewsDB.identityView.find().explain("queryPlanner"));
explainPlan = getSingleNodeExplain(explainPlan);
assert.eq(explainPlan.queryPlanner.namespace, "views_find.coll");
assert(!explainPlan.hasOwnProperty("executionStats"));

explainPlan = assert.commandWorked(viewsDB.identityView.find().explain("executionStats"));
explainPlan = getSingleNodeExplain(explainPlan);
assert.eq(explainPlan.queryPlanner.namespace, "views_find.coll");
assert(explainPlan.hasOwnProperty("executionStats"));
assert.eq(explainPlan.executionStats.nReturned, 5);
assert(!explainPlan.executionStats.hasOwnProperty("allPlansExecution"));

explainPlan = assert.commandWorked(viewsDB.identityView.find().explain("allPlansExecution"));
explainPlan = getSingleNodeExplain(explainPlan);
assert.eq(explainPlan.queryPlanner.namespace, "views_find.coll");
assert(explainPlan.hasOwnProperty("executionStats"));
assert.eq(explainPlan.executionStats.nReturned, 5);
assert(explainPlan.executionStats.hasOwnProperty("allPlansExecution"));

// Only simple 0 or 1 projections are allowed on views.
assert.commandWorked(viewsDB.coll.insert({arr: [{x: 1}]}));
assert.commandFailedWithCode(
    viewsDB.runCommand({find: "identityView", projection: {arr: {$elemMatch: {x: 1}}}}),
    ErrorCodes.InvalidPipelineOperator);

// Views can support a "findOne" if singleBatch: true and limit: 1.
let res = assert.commandWorked(
    viewsDB.runCommand({find: "identityView", filter: {state: "NY"}, singleBatch: true, limit: 1}));
assert.eq(res.cursor.firstBatch, [{_id: "New York", state: "NY", pop: 7}]);
// singleBatch: true should ensure no cursor is returned.
assert.eq(res.cursor.id, 0);
// The behavior should be the same with batchSize: 1.
res = assert.commandWorked(viewsDB.runCommand(
    {find: "identityView", filter: {}, singleBatch: true, limit: 1, batchSize: 1}));
assert.eq(res.cursor.id, 0);
assert.eq(viewsDB.identityView.findOne({_id: "San Francisco"}),
          {_id: "San Francisco", state: "CA", pop: 4});

// The readOnce cursor option is not allowed on views.  But if we're in a transaction,
// the error code saying that it's not allowed in a transaction takes precedence.
assert.commandFailedWithCode(
    viewsDB.runCommand({find: "identityView", readOnce: true}),
    [ErrorCodes.OperationNotSupportedInTransaction, ErrorCodes.InvalidPipelineOperator]);
