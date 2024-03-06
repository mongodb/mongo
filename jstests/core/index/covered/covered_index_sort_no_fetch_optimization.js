/**
 * Confirms that blocking sorts are covered when the index contains the sort key. For example, if we
 * have an index on {a:1, b:1} and a sort on {b:1}, and a projection of only field 'b', we can sort
 * using only the existing index keys, without needing to do a fetch.
 *
 * Queries on a sharded collection can't be covered when they aren't on the shard key. The document
 * must be fetched to support the SHARDING_FILTER stage.
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
import {getWinningPlan, isIndexOnly, planHasStage} from "jstests/libs/analyze_plan.js";

const collName = "covered_index_sort_no_fetch_optimization";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.createIndex({a: 1, b: 1}));

assert.commandWorked(coll.insert([
    {a: 1, b: 1, c: 1},
    {a: 1, b: 2, c: 2},
    {a: 2, b: 1, c: 3},
    {a: 2, b: 2, c: 4},
    {a: -1, b: 1, c: 5}
]));

const kIsCovered = true;
const kNotCovered = false;
const kBlockingSort = true;
const kNonBlockingSort = false;

function assertExpectedResult(findCmd, expectedResult, isCovered, isBlockingSort) {
    const result = assert.commandWorked(db.runCommand(findCmd));
    assert.eq(result.cursor.firstBatch, expectedResult, result);

    const explainResult =
        assert.commandWorked(db.runCommand({explain: findCmd, verbosity: "executionStats"}));
    assert.eq(
        isCovered, isIndexOnly(db, getWinningPlan(explainResult.queryPlanner)), explainResult);
    assert.eq(isBlockingSort,
              planHasStage(db, getWinningPlan(explainResult.queryPlanner), "SORT"),
              explainResult);
}

// Test correctness of basic covered queries. Here, the sort predicate is not the same order
// as the index order, but uses the same keys.
let findCmd = {find: collName, filter: {a: {$lt: 2}}, projection: {b: 1, _id: 0}, sort: {b: 1}};
let expected = [{"b": 1}, {"b": 1}, {"b": 2}];
assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);

findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {a: 1, b: 1, _id: 0},
    sort: {b: 1, a: 1}
};
expected = [{"a": 1, "b": 1}, {"a": 2, "b": 1}, {"a": 1, "b": 2}, {"a": 2, "b": 2}];
assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);

findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {a: 1, b: 1, _id: 0},
    sort: {b: 1, a: -1}
};
expected = [{"a": 2, "b": 1}, {"a": 1, "b": 1}, {"a": 2, "b": 2}, {"a": 1, "b": 2}];
assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);

// Test correctness of queries where sort is not covered because not all sort keys are in the
// index.
findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {b: 1, c: 1, _id: 0},
    sort: {c: 1, b: 1}
};
expected = [{"b": 1, "c": 1}, {"b": 2, "c": 2}, {"b": 1, "c": 3}, {"b": 2, "c": 4}];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {b: 1, _id: 0},
    sort: {c: 1, b: 1}
};
expected = [{"b": 1}, {"b": 2}, {"b": 1}, {"b": 2}];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

// When the sort key is multikey, we cannot cover the sort using the index.
assert.commandWorked(coll.insert({a: 1, b: [4, 5, 6]}));
assert.commandWorked(coll.insert({a: 1, b: [-1, 11, 12]}));
findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {b: 1, _id: 0},
    sort: {b: 1}
};
expected = [{"b": [-1, 11, 12]}, {"b": 1}, {"b": 1}, {"b": 2}, {"b": 2}, {"b": [4, 5, 6]}];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

// Collation Tests.

// If you have an index with the same index key pattern and the same collation as the sort key,
// then no blocking sort is required.
assert(coll.drop());
// Note that {locale: "en_US", strength: 3} differ from the simple collation with respect to
// case ordering. "en_US" collation puts lowercase letters first, whereas the simple collation
// puts uppercase first.
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {collation: {locale: "en_US", strength: 3}}));
assert.commandWorked(
    coll.insert([{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "A"}, {a: 1, b: "a"}, {a: 2, b: 2}]));

findCmd = {
    find: collName,
    filter: {},
    projection: {a: 1, b: 1, _id: 0},
    collation: {locale: "en_US", strength: 3},
    sort: {a: 1, b: 1},
    hint: {a: 1, b: 1}
};
expected =
    [{"a": 1, "b": 1}, {"a": 1, "b": 2}, {"a": 1, "b": "a"}, {"a": 1, "b": "A"}, {"a": 2, "b": 2}];
assertExpectedResult(findCmd, expected, kNotCovered, kNonBlockingSort);

// This tests the case where there is a collation, and we need to do a blocking SORT, but that
// SORT could be computed using the index keys. However, this query cannot be covered due the
// index having a non-simple collation.
findCmd = {
    find: collName,
    filter: {a: {$lt: 2}},
    projection: {b: 1, _id: 0},
    collation: {locale: "en_US", strength: 3},
    sort: {b: 1},
    hint: {a: 1, b: 1}
};
expected = [
    {"b": 1},
    {"b": 2},
    {"b": "a"},
    {"b": "A"},
];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

// The index has the same key pattern as the sort but a different collation.
// We expect to add a fetch stage here as 'b' is not guaranteed to be in the correct order.
assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {collation: {locale: "en_US", strength: 1}}));

findCmd = {
    find: collName,
    filter: {},
    projection: {a: 1, b: 1, _id: 0},
    collation: {locale: "en_US", strength: 3},
    sort: {a: 1, b: 1},
    hint: {a: 1, b: 1}
};
expected = [{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "a"}, {a: 1, b: "A"}, {a: 2, b: 2}];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

// The index has a collation but the query sort does not.
// We expect to add a fetch stage here as 'b' is not guaranteed to be in the correct order.
assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {collation: {locale: "en_US", strength: 3}}));
findCmd = {
    find: collName,
    filter: {},
    projection: {a: 1, b: 1, _id: 0},
    sort: {a: 1, b: 1},
    hint: {a: 1, b: 1}
};
expected = [{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "A"}, {a: 1, b: "a"}, {a: 2, b: 2}];
assertExpectedResult(findCmd, expected, kNotCovered, kBlockingSort);

// The index has a collation but the query does not. However, our index bounds do not contain
// strings, so we can apply the no-fetch optimization.
findCmd = {
    find: collName,
    filter: {a: {$gte: 1}, b: 2},
    projection: {a: 1, b: 1, _id: 0},
    sort: {b: 1, a: 1},
    hint: {a: 1, b: 1}
};
expected = [{a: 1, b: 2}, {a: 2, b: 2}];
assertExpectedResult(findCmd, expected, kIsCovered, kNonBlockingSort);

// The index does not have a special collation, but the query asks for one. The no-fetch
// optimization will be applied in this case. The server must correctly respect the collation
// when sorting the index keys, as the index keys do not already reflect the collation.
assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on the cursor_hints_to_query_settings
    // suite. The suite replaces cursor hints with query settings. Query settings do not force
    // indexes, and therefore empty filter will result in collection scans.
    findCmd = {
        find: collName,
        filter: {},
        projection: {a: 1, b: 1, _id: 0},
        collation: {locale: "en_US", strength: 3},
        sort: {a: 1, b: 1},
        hint: {a: 1, b: 1}
    };

    expected = [{a: 1, b: 1}, {a: 1, b: 2}, {a: 1, b: "a"}, {a: 1, b: "A"}, {a: 2, b: 2}];
    assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);
}

// Test covered sort plan possible with non-multikey dotted field in sort key.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
assert.commandWorked(coll.insert([
    {a: 0, b: {c: 1}},
    {a: 1, b: {c: 2}},
    {a: 2, b: {c: "A"}},
    {a: 3, b: {c: "a"}},
    {a: 4, b: {c: 3}}
]));

findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {a: 1, "b.c": 1, _id: 0},
    sort: {"b.c": 1}
};
expected = [
    {"a": 1, "b": {"c": 2}},
    {"a": 4, "b": {"c": 3}},
    {"a": 2, "b": {"c": "A"}},
    {"a": 3, "b": {"c": "a"}}
];
assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);

assert.commandWorked(coll.insert({a: [1], b: {c: 1}}));
findCmd = {
    find: collName,
    filter: {a: {$gt: 0}},
    projection: {"b.c": 1, _id: 0},
    sort: {"b.c": 1}
};
expected =
    [{"b": {"c": 1}}, {"b": {"c": 2}}, {"b": {"c": 3}}, {"b": {"c": "A"}}, {"b": {"c": "a"}}];
assertExpectedResult(findCmd, expected, kIsCovered, kBlockingSort);
