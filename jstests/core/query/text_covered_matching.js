//
// When a $text query includes an additional predicate that can be covered with a suffix of a $text
// index, we expect the query planner to attach that predicate as a "filter" to the TEXT_OR or OR or
// IXSCAN stage, so that it can be used to filter non-matching documents without fetching them.
//
// SERVER-26833 changes how the text index is searched in the case when the projection does not
// include the 'textScore' meta field, so we are adding this test to ensure that we still get the
// same covered matching behavior with and without 'textScore' in the projection.
//
// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
//   # $text is not supported on views.
//   incompatible_with_views,
// ]

import {getPlanStages, getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

const coll = db.text_covered_matching;

coll.drop();
assert.commandWorked(coll.createIndex({a: "text", b: 1}));
assert.commandWorked(coll.insert({a: "hello", b: 1, c: 1}));
assert.commandWorked(coll.insert({a: "world", b: 2, c: 2}));
assert.commandWorked(coll.insert({a: "hello world", b: 3, c: 3}));

//
// Test the query {$text: {$search: "hello"}, b: 1} with and without the 'textScore' in the
// output.
//

// Expected result:
//   - We examine two keys, for the two documents with "hello" in their text;
//   - we examine only one document, because covered matching rejects the index entry for
//     which b != 1;
//   - we return exactly one document.
let explainResult = coll.find({$text: {$search: "hello"}, b: 1}).explain("executionStats");
assert.commandWorked(explainResult);
assert(!planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert(!planHasStage(db, getWinningPlanFromExplain(explainResult), "OR"));
assert(planHasStage(db, getWinningPlanFromExplain(explainResult), "IXSCAN"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.eq(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));
// If we are executing against a mongos, we have more than one occurrence of IXSCAN.
let filteringStage = getPlanStages(explainResult, "IXSCAN")[0];
assert.hasFields(filteringStage, ["filter"], "No filter found on IXSCAN: " + tojson(filteringStage));
assert.docEq({"b": {"$eq": 1}}, filteringStage.filter, "Incorrect filter on IXSCAN.");

// When we include the text score in the projection, we use a TEXT_OR in our query plan, which
// changes how filtering is done. We should get the same result, however.
explainResult = coll
    .find({$text: {$search: "hello"}, b: 1}, {a: 1, b: 1, c: 1, textScore: {$meta: "textScore"}})
    .explain("executionStats");
assert.commandWorked(explainResult);
assert(planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.eq(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));
filteringStage = getPlanStages(explainResult, "TEXT_OR")[0];
assert.hasFields(filteringStage, ["filter"], "No filter found on TEXT_OR: " + tojson(filteringStage));
assert.docEq({"b": {"$eq": 1}}, filteringStage.filter, "Incorrect filter on TEXT_OR.");

// When we search more than one term, we perform filtering in the OR stage rather than the
// underlying IXSCANs, but we should get an equivalent result.
explainResult = coll.find({$text: {$search: "hello world"}, b: 1}).explain("executionStats");
assert.commandWorked(explainResult);
assert(planHasStage(db, getWinningPlanFromExplain(explainResult), "OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    4,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.eq(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));
filteringStage = getPlanStages(explainResult, "OR")[0];
assert.hasFields(filteringStage, ["filter"], "No filter found on OR: " + tojson(filteringStage));
assert.docEq({"b": {"$eq": 1}}, filteringStage.filter, "Incorrect filter on OR.");

//
// Test the query {$text: {$search: "hello"}, c: 1} with and without the 'textScore' in the
// output.
//

// Expected result:
//   - We examine two keys, for the two documents with "hello" in their text;
//   - we examine more than just the matching document, because we need to fetch documents in
//     order to examine the non-covered 'c' field;
//   - we return exactly one document.
explainResult = coll.find({$text: {$search: "hello"}, c: 1}).explain("executionStats");
assert.commandWorked(explainResult);
assert(!planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.gt(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));

// As before, including the text score in the projection changes how filtering occurs, but we
// still expect the same result.
explainResult = coll
    .find({$text: {$search: "hello"}, c: 1}, {a: 1, b: 1, c: 1, textScore: {$meta: "textScore"}})
    .explain("executionStats");
assert.commandWorked(explainResult);
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.gt(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));

//
// Test the first query again, but this time, use dotted fields to make sure they don't confuse
// the query planner:
//   {$text: {$search: "hello"}, "b.d": 1}
//
coll.drop();
assert.commandWorked(coll.createIndex({a: "text", "b.d": 1}));
assert.commandWorked(coll.insert({a: "hello", b: {d: 1}, c: {e: 1}}));
assert.commandWorked(coll.insert({a: "world", b: {d: 2}, c: {e: 2}}));
assert.commandWorked(coll.insert({a: "hello world", b: {d: 3}, c: {e: 3}}));

// Expected result:
//   - We examine two keys, for the two documents with "hello" in their text;
//   - we examine only one document, because covered matching rejects the index entry for
//     which b != 1;
//   - we return exactly one document.
explainResult = coll.find({$text: {$search: "hello"}, "b.d": 1}).explain("executionStats");
assert.commandWorked(explainResult);
assert(!planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.eq(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));

// When we include the text score in the projection, we use a TEXT_OR in our query plan, which
// changes how filtering is done. We should get the same result, however.
explainResult = coll
    .find({$text: {$search: "hello"}, "b.d": 1}, {a: 1, b: 1, c: 1, textScore: {$meta: "textScore"}})
    .explain("executionStats");
assert.commandWorked(explainResult);
assert(planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.eq(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));

//
// Test the second query again, this time with dotted fields:
//   {$text: {$search: "hello"}, "c.e": 1}
//

// Expected result:
//   - We examine two keys, for the two documents with "hello" in their text;
//   - we examine more than just the matching document, because we need to fetch documents in
//     order to examine the non-covered 'c' field;
//   - we return exactly one document.
explainResult = coll.find({$text: {$search: "hello"}, "c.e": 1}).explain("executionStats");
assert.commandWorked(explainResult);
assert(!planHasStage(db, getWinningPlanFromExplain(explainResult), "TEXT_OR"));
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.gt(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));

// As before, including the text score in the projection changes how filtering occurs, but we
// still expect the same result.
explainResult = coll
    .find({$text: {$search: "hello"}, "c.e": 1}, {a: 1, b: 1, c: 1, textScore: {$meta: "textScore"}})
    .explain("executionStats");
assert.commandWorked(explainResult);
assert.eq(
    explainResult.executionStats.totalKeysExamined,
    2,
    "Unexpected number of keys examined: " + tojson(explainResult),
);
assert.gt(
    explainResult.executionStats.totalDocsExamined,
    1,
    "Unexpected number of documents examined: " + tojson(explainResult),
);
assert.eq(explainResult.executionStats.nReturned, 1, "Unexpected number of results returned: " + tojson(explainResult));
