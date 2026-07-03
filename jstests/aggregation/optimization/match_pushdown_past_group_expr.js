/**
 * Tests that $match pushdown past $group preserves result correctness when $expr predicates are
 * involved.
 *
 * The test implicitly depends on the $group processing order, since $group forms buckets based
 * on the first-seen type of value:
 * If it sees [NumberInt(1), NumberLong(1)] -> group as NumberInt(1);
 * If it sees [NumberLong(1), NumberInt(1)] -> group as NumberLong(1).
 *
 * @tags: [
 *  # Sharded $group merges per-shard partial results whose _id types are non-deterministic.
 *  assumes_unsharded_collection,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(
    coll.insertMany([
        {_id: 1, a: null},
        {_id: 2},
        {_id: 3, a: NumberInt(1)},
        {_id: 4, a: NumberLong(1)},
    ]),
);

// Ensure stable processing order is seen by $group.
const sortStage = {$sort: {_id: 1}};
// After $group we should have:
// - {_id: null, n: 2}
// - {_id: NumberInt(1), n: 2}
const groupStage = {$group: {_id: "$a", n: {$count: {}}}};

function runTest({name, matchStage, expected} = {}) {
    it(name, () => {
        const optimized = coll.aggregate([sortStage, groupStage, matchStage]).toArray();
        const unoptimized = coll
            .aggregate([sortStage, groupStage, {$_internalInhibitOptimization: {}}, matchStage])
            .toArray();
        assertArrayEq({
            actual: optimized,
            expected: unoptimized,
            extraErrorMsg: `\noptimized=${tojson(optimized)}, unoptimized=${tojson(unoptimized)}`,
        });
        assertArrayEq({actual: unoptimized, expected});
    });
}

describe("comparisons with non-null constants", function () {
    runTest({
        name: "$eq",
        matchStage: {$match: {$expr: {$eq: ["$_id", 1]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$gt",
        matchStage: {$match: {$expr: {$gt: ["$_id", 1]}}},
        expected: [],
    });

    runTest({
        name: "$gte",
        matchStage: {$match: {$expr: {$gte: ["$_id", 1]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$lt",
        matchStage: {$match: {$expr: {$lt: ["$_id", 1]}}},
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$lte",
        matchStage: {$match: {$expr: {$lte: ["$_id", 1]}}},
        expected: [
            {_id: null, n: 2},
            {_id: 1, n: 2},
        ],
    });
});

describe("comparisons with null", function () {
    runTest({
        name: "$eq null",
        matchStage: {$match: {$expr: {$eq: ["$_id", null]}}},
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$gt null",
        matchStage: {$match: {$expr: {$gt: ["$_id", null]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$gte null",
        matchStage: {$match: {$expr: {$gte: ["$_id", null]}}},
        expected: [
            {_id: null, n: 2},
            {_id: 1, n: 2},
        ],
    });

    runTest({
        name: "$lt null",
        matchStage: {$match: {$expr: {$lt: ["$_id", null]}}},
        expected: [],
    });

    runTest({
        name: "$lte null",
        matchStage: {$match: {$expr: {$lte: ["$_id", null]}}},
        expected: [{_id: null, n: 2}],
    });
});

describe("logical operators on comparisons", function () {
    runTest({
        name: "$and",
        matchStage: {$match: {$expr: {$and: [{$gt: ["$_id", 0]}, {$lt: ["$_id", 5]}]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$not of $or",
        matchStage: {$match: {$expr: {$not: {$or: [{$eq: ["$_id", 1]}, {$gt: ["$_id", 5]}]}}}},
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$not of $ne null",
        matchStage: {$match: {$expr: {$not: {$ne: ["$_id", null]}}}},
        expected: [{_id: null, n: 2}],
    });
});

describe("logic operators above $expr", function () {
    runTest({
        name: "$and",
        matchStage: {$match: {$and: [{$expr: {$gt: ["$_id", 0]}}, {$expr: {$lt: ["$_id", 5]}}]}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$and with null comparison",
        matchStage: {
            $match: {$and: [{$expr: {$gte: ["$_id", null]}}, {$expr: {$lte: ["$_id", null]}}]},
        },
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$or",
        matchStage: {$match: {$or: [{$expr: {$eq: ["$_id", 1]}}, {$expr: {$gt: ["$_id", 5]}}]}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$or with null comparison",
        matchStage: {$match: {$or: [{$expr: {$eq: ["$_id", null]}}, {$expr: {$gt: ["$_id", 5]}}]}},
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$nor",
        matchStage: {
            $match: {$nor: [{$expr: {$eq: ["$_id", 1]}}, {$expr: {$gt: ["$_id", 5]}}]},
        },
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$nor with null comparison",
        matchStage: {$match: {$nor: [{$expr: {$eq: ["$_id", null]}}]}},
        expected: [{_id: 1, n: 2}],
    });
});

describe("$in", function () {
    runTest({
        name: "non-null constant array",
        matchStage: {$match: {$expr: {$in: ["$_id", [1, 2]]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "null element in array",
        matchStage: {$match: {$expr: {$in: ["$_id", [null]]}}},
        expected: [{_id: null, n: 2}],
    });
});

describe("complex expressions", function () {
    runTest({
        name: "$cond with null comparison",
        matchStage: {
            $match: {$expr: {$cond: {if: {$eq: ["$_id", null]}, then: true, else: false}}},
        },
        expected: [{_id: null, n: 2}],
    });

    runTest({
        name: "$type check on original value",
        matchStage: {$match: {$expr: {$eq: ["int", {$type: "$_id"}]}}},
        expected: [{_id: 1, n: 2}],
    });

    runTest({
        name: "$type check on computed value",
        matchStage: {$match: {$expr: {$eq: ["int", {$type: {$add: ["$_id", NumberInt(1)]}}]}}},
        expected: [{_id: 1, n: 2}],
    });
});

it("$expr against a $lookup let variable is evaluated per document", () => {
    // Regression test for bug which resulted in the let variable in the shared pipeline used
    // by $lookup being optimized away and affecting the caching logic.
    const local = db[jsTestName() + "_local"];
    const foreign = db[jsTestName() + "_foreign"];
    local.drop();
    foreign.drop();

    assert.commandWorked(foreign.insertMany([{_id: 0}, {_id: 1}]));
    assert.commandWorked(local.insertMany([{_id: 0}, {_id: 1}]));

    const groupOnForeign = {$group: {_id: "$_id"}};
    const matchOnLetVar = {$match: {$expr: {$eq: ["$_id", {$add: ["$$sel", 0]}]}}};

    function lookup(subPipeline) {
        return {
            $lookup: {
                from: foreign.getName(),
                let: {sel: "$_id"},
                pipeline: subPipeline,
                as: "matched",
            },
        };
    }

    const optimized = local.aggregate([lookup([groupOnForeign, matchOnLetVar])]).toArray();
    const unoptimized = local
        .aggregate([lookup([groupOnForeign, {$_internalInhibitOptimization: {}}, matchOnLetVar])])
        .toArray();
    assertArrayEq({
        actual: optimized,
        expected: unoptimized,
        extraErrorMsg: `\noptimized=${tojson(optimized)}, unoptimized=${tojson(unoptimized)}`,
    });
    assertArrayEq({
        actual: unoptimized,
        expected: [
            {_id: 0, matched: [{_id: 0}]},
            {_id: 1, matched: [{_id: 1}]},
        ],
    });
});
