/**
 * Test the distinct command with views.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   requires_fcv_71,
 * ]
 */

// For arrayEq. We don't use array.eq as it does an ordered comparison on arrays but we don't
// care about order in the distinct response.
import {arrayEq} from "jstests/aggregation/extras/utils.js";
import {getAllNodeExplains, getPlanStage, getWinningPlan} from "jstests/libs/analyze_plan.js";

var viewsDB = db.getSiblingDB("views_distinct");
assert.commandWorked(viewsDB.dropDatabase());

// Populate a collection with some test data.
let allDocuments = [];
allDocuments.push({_id: "New York", state: "NY", pop: 7});
allDocuments.push({_id: "Newark", state: "NJ", pop: 3});
allDocuments.push({_id: "Palo Alto", state: "CA", pop: 10});
allDocuments.push({_id: "San Francisco", state: "CA", pop: 4});
allDocuments.push({_id: "Trenton", state: "NJ", pop: 5});

let coll = viewsDB.getCollection("coll");
let bulk = coll.initializeUnorderedBulkOp();
allDocuments.forEach(function(doc) {
    bulk.insert(doc);
});
assert.commandWorked(bulk.execute());

// Create views on the data.
assert.commandWorked(viewsDB.runCommand({create: "identityView", viewOn: "coll"}));
assert.commandWorked(viewsDB.runCommand(
    {create: "largePopView", viewOn: "identityView", pipeline: [{$match: {pop: {$gt: 5}}}]}));
let identityView = viewsDB.getCollection("identityView");
let largePopView = viewsDB.getCollection("largePopView");

function assertIdentityViewDistinctMatchesCollection(key, query) {
    query = (query === undefined) ? {} : query;
    const collDistinct = coll.distinct(key, query);
    const viewDistinct = identityView.distinct(key, query);
    assert(arrayEq(collDistinct, viewDistinct),
           "Distinct on a collection did not match distinct on its identity view; got " +
               tojson(viewDistinct) + " but expected " + tojson(collDistinct));
}

// Test basic distinct requests on known fields without a query.
assertIdentityViewDistinctMatchesCollection("pop");
assertIdentityViewDistinctMatchesCollection("_id");
assert(arrayEq([7, 10], largePopView.distinct("pop")));
assert(arrayEq(["New York", "Palo Alto"], largePopView.distinct("_id")));

// Test distinct with the presence of a query.
assertIdentityViewDistinctMatchesCollection("state", {});
assertIdentityViewDistinctMatchesCollection("pop", {pop: {$exists: true}});
assertIdentityViewDistinctMatchesCollection("state", {pop: {$gt: 3}});
assertIdentityViewDistinctMatchesCollection("_id", {state: "CA"});
assert(arrayEq(["CA"], largePopView.distinct("state", {pop: {$gte: 8}})));
assert(arrayEq([7], largePopView.distinct("pop", {state: "NY"})));

// Test distinct where we expect an empty set response.
assertIdentityViewDistinctMatchesCollection("nonexistent");
assertIdentityViewDistinctMatchesCollection("pop", {pop: {$gt: 1000}});
assert.eq([], largePopView.distinct("nonexistent"));
assert.eq([], largePopView.distinct("_id", {state: "FL"}));

// Explain works with distinct.
assert.commandWorked(identityView.explain().distinct("_id"));
assert.commandWorked(largePopView.explain().distinct("pop", {state: "CA"}));
getAllNodeExplains(largePopView.explain().count({foo: "bar"})).forEach((explainPlan) => {
    if (explainPlan.hasOwnProperty("stages") && explainPlan.stages[0].hasOwnProperty('$cursor')) {
        explainPlan = explainPlan.stages[0].$cursor;
    }
    assert.eq(explainPlan.queryPlanner.namespace, "views_distinct.coll");
});

// Distinct with explicit explain modes works on a view.
getAllNodeExplains(assert.commandWorked(largePopView.explain("queryPlanner").distinct("pop")))
    .forEach((explainPlan) => {
        assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace, "views_distinct.coll");
        assert(!explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"));
    });

let nReturned = 0;
getAllNodeExplains(largePopView.explain("executionStats").distinct("pop"))
    .forEach((explainPlan) => {
        assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace, "views_distinct.coll");
        assert(explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"));
        nReturned += explainPlan.stages[0].$cursor.executionStats.nReturned;
        assert(!explainPlan.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"));
    });
assert.eq(nReturned, 2);

nReturned = 0;
getAllNodeExplains(largePopView.explain("allPlansExecution").distinct("pop"))
    .forEach((explainPlan) => {
        assert.eq(explainPlan.stages[0].$cursor.queryPlanner.namespace, "views_distinct.coll");
        assert(explainPlan.stages[0].$cursor.hasOwnProperty("executionStats"));
        nReturned += explainPlan.stages[0].$cursor.executionStats.nReturned;
        assert(explainPlan.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"));
    });
assert.eq(nReturned, 2);

// Distinct with hints work on views.
assert.commandWorked(viewsDB.coll.createIndex({state: 1}));

getAllNodeExplains(largePopView.explain().distinct("pop", {}, {
    hint: {state: 1}
})).forEach((explainPlan) => {
    assert(getPlanStage(explainPlan.stages[0].$cursor, "FETCH"));
    assert(getPlanStage(explainPlan.stages[0].$cursor, "IXSCAN"));
});

getAllNodeExplains(largePopView.explain().distinct("pop")).forEach((explainPlan) => {
    assert.neq(getWinningPlan(explainPlan.stages[0].$cursor.queryPlanner).stage,
               "IXSCAN",
               tojson(explainPlan));
});

// Make sure that the hint produces the right results.
assert(arrayEq([10, 7], largePopView.distinct("pop", {}, {hint: {state: 1}})));
const result =
    largePopView.runCommand("distinct", {"key": "a", query: {a: 1, b: 2}, hint: {bad: 1, hint: 1}});
assert.commandFailedWithCode(result, ErrorCodes.BadValue, result);
const regex = new RegExp("hint provided does not correspond to an existing index");
assert(regex.test(result.errmsg));

// Distinct commands fail when they try to change the collation of a view.
assert.commandFailedWithCode(
    viewsDB.runCommand({distinct: "identityView", key: "state", collation: {locale: "en_US"}}),
    ErrorCodes.OptionNotSupportedOnView);

// Test distinct on nested objects, nested arrays and nullish values.
coll.drop();
allDocuments = [];
allDocuments.push({a: 1, b: [2, 3, [4, 5], {c: 6}], d: {e: [1, 2]}});
allDocuments.push({a: [1], b: [2, 3, 4, [5]], c: 6, d: {e: 1}});
allDocuments.push({a: [[1]], b: [2, 3, [4], [5]], c: 6, d: [[{e: 1}]]});
allDocuments.push({a: [[1]], b: [2, 3, [4], [5]], c: 6, d: [{e: {f: 1}}]});
allDocuments.push({a: [[1]], b: [2, 3, [4], [5]], c: 6, d: {e: [[{f: 1}]]}});
allDocuments.push({a: [1, 2], b: 3, c: [6], d: [{e: 1}, {e: [1, 2]}, {e: {someObject: 1}}]});
allDocuments.push({a: [1, 2], b: [4, 5], c: [undefined], d: [1]});
allDocuments.push({a: null, b: [4, 5, null, undefined], c: [], d: {e: null}});
allDocuments.push({a: undefined, b: null, c: [null], d: {e: undefined}});

bulk = coll.initializeUnorderedBulkOp();
allDocuments.forEach(function(doc) {
    bulk.insert(doc);
});
assert.commandWorked(bulk.execute());

assertIdentityViewDistinctMatchesCollection("a");
assertIdentityViewDistinctMatchesCollection("b");
assertIdentityViewDistinctMatchesCollection("c");
assertIdentityViewDistinctMatchesCollection("d");
assertIdentityViewDistinctMatchesCollection("e");
assertIdentityViewDistinctMatchesCollection("d.e");
assertIdentityViewDistinctMatchesCollection("d.e.f");

// Test distinct on a deeply nested object through arrays.
coll.drop();
assert.commandWorked(coll.insert({
    a: [
        {b: [{c: [{d: 1}]}]},
        {b: {c: "not leaf"}},
        {b: {c: [{d: 2, "not leaf": "not leaf"}]}},
        {b: [{c: {d: 3}}]},
        {b: {c: {d: 4}}, "not leaf": "not leaf"},
        "not leaf",
        // The documents below should not get traversed by the distinct() because of the
        // doubly-nested arrays.
        [[{b: {c: {d: "not leaf"}}}]],
        [{b: {c: [[{d: "not leaf"}]]}}],
    ]
}));
assert.commandWorked(coll.insert({a: "not leaf"}));
assertIdentityViewDistinctMatchesCollection("a");
assertIdentityViewDistinctMatchesCollection("a.b");
assertIdentityViewDistinctMatchesCollection("a.b.c");
assertIdentityViewDistinctMatchesCollection("a.b.c.d");
