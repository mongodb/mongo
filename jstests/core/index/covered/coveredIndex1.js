/**
 * Tests queries that are covered by an index.
 *
 * This test cannot implicitly shard accessed collections because queries on a sharded collection
 * cannot be covered when they aren't on the shard key since the document needs to be fetched in
 * order to apply the SHARDING_FILTER stage.
 * @tags: [
 *   assumes_unsharded_collection,
 *   # The test assumes it is in control of which indexes exist and makes some assertions on explain
 *   # plans.
 *   assumes_no_implicit_index_creation,
 * ]
 */
// Include helpers for analyzing explain output.
import {getWinningPlanFromExplain, isIndexOnly} from "jstests/libs/query/analyze_plan.js";

const coll = db["jstests_coveredIndex1"];
coll.drop();

assert.commandWorked(coll.insert({order: 0, fn: "john", ln: "doe"}));
assert.commandWorked(coll.insert({order: 1, fn: "jack", ln: "doe"}));
assert.commandWorked(coll.insert({order: 2, fn: "john", ln: "smith"}));
assert.commandWorked(coll.insert({order: 3, fn: "jack", ln: "black"}));
assert.commandWorked(coll.insert({order: 4, fn: "bob", ln: "murray"}));
assert.commandWorked(coll.insert({order: 5, fn: "aaa", ln: "bbb", obj: {a: 1, b: "blah"}}));
assert.commandWorked(coll.insert({order: 6, fn: "ccc", ln: "ddd", arr: [1, 2, 3, 4, 5, 6]}));

/**
 * Asserts that running the find command with query 'query' and projection 'projection' is
 * covered if 'isCovered' is true, or not covered otherwise.
 *
 * If 'hint' is specified, use 'hint' as the suggested index.
 */
function assertIfQueryIsCovered(query, projection, isCovered, hint) {
    let cursor = coll.find(query, projection);
    if (hint) {
        cursor = cursor.hint(hint);
    }
    const explain = cursor.explain();
    assert.commandWorked(explain);

    assert(explain.hasOwnProperty("queryPlanner"), tojson(explain));
    assert(explain.queryPlanner.hasOwnProperty("winningPlan"), tojson(explain));
    const winningPlan = getWinningPlanFromExplain(explain);
    if (isCovered) {
        assert(
            isIndexOnly(db, winningPlan),
            "Query " +
                tojson(query) +
                " with projection " +
                tojson(projection) +
                " should have been covered, but got this plan: " +
                tojson(winningPlan),
        );
    } else {
        assert(
            !isIndexOnly(db, winningPlan),
            "Query " +
                tojson(query) +
                " with projection " +
                tojson(projection) +
                " should not have been covered, but got this plan: " +
                tojson(winningPlan),
        );
    }
}

// Create an index on one field.
assert.commandWorked(coll.createIndex({ln: 1}));
assertIfQueryIsCovered({}, {}, false);
assertIfQueryIsCovered({ln: "doe"}, {}, false);
assertIfQueryIsCovered({ln: "doe"}, {ln: 1}, false);
assertIfQueryIsCovered({ln: "doe"}, {ln: 1, _id: 0}, true, {ln: 1});

// Create a compound index.
assert.commandWorked(coll.dropIndex({ln: 1}));
assert.commandWorked(coll.createIndex({ln: 1, fn: 1}));
assertIfQueryIsCovered({ln: "doe"}, {ln: 1, _id: 0}, true);
assertIfQueryIsCovered({ln: "doe"}, {ln: 1, fn: 1, _id: 0}, true);
assertIfQueryIsCovered({ln: "doe", fn: "john"}, {ln: 1, fn: 1, _id: 0}, true);
assertIfQueryIsCovered({fn: "john", ln: "doe"}, {fn: 1, ln: 1, _id: 0}, true);
assertIfQueryIsCovered({fn: "john"}, {fn: 1, _id: 0}, false);

// Repeat the above test, but with a compound index involving _id.
assert.commandWorked(coll.dropIndex({ln: 1, fn: 1}));
assert.commandWorked(coll.createIndex({_id: 1, ln: 1}));
assertIfQueryIsCovered({_id: 123, ln: "doe"}, {_id: 1}, true);
assertIfQueryIsCovered({_id: 123, ln: "doe"}, {ln: 1}, true);
assertIfQueryIsCovered({ln: "doe", _id: 123}, {ln: 1, _id: 1}, true);
assertIfQueryIsCovered({ln: "doe"}, {ln: 1}, false);

// Create an index on an embedded object.
assert.commandWorked(coll.dropIndex({_id: 1, ln: 1}));
assert.commandWorked(coll.createIndex({obj: 1}));
assertIfQueryIsCovered({"obj.a": 1}, {obj: 1}, false);
assertIfQueryIsCovered({obj: {a: 1, b: "blah"}}, false);
assertIfQueryIsCovered({obj: {a: 1, b: "blah"}}, {obj: 1, _id: 0}, true);

// Create indexes on fields inside an embedded object.
assert.commandWorked(coll.dropIndex({obj: 1}));
assert.commandWorked(coll.createIndex({"obj.a": 1, "obj.b": 1}));
assertIfQueryIsCovered({"obj.a": 1}, {obj: 1}, false);

// Create indexes on single array elements.
assert.commandWorked(coll.dropIndex({"obj.a": 1, "obj.b": 1}));
assert.commandWorked(coll.createIndex({"arr.0": 1}));
assertIfQueryIsCovered({"arr.0": 1}, {}, false);
assertIfQueryIsCovered({"arr.0": {$mod: [7, 6]}}, {}, false);
