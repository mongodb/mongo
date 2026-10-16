/**
 * Tests that a $expr + $in match expression respects the collation, whether the collation is
 * supplied with the query or comes from the collection default. The rewrite of $expr + $in into a
 * MatchExpression $in (SERVER-32549) did not propagate the collator, so a case-insensitive
 * collation matched only documents with the exact case (SERVER-132644).
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_getmore,
 *   # Views do not inherit default collection collation.
 *   incompatible_with_views,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const caseInsensitive = {locale: "en", strength: 1};
const docs = [
    {_id: 0, val: "apple"},
    {_id: 1, val: "Apple"},
    {_id: 2, val: "APPLE"},
];

// Each entry runs against both the query-supplied and the collection-default collation.
const testCases = [
    // The bug: $expr + $in compared strings exactly and returned only {_id: 1}.
    {filter: {$expr: {$in: ["$val", ["Apple"]]}}, expected: [0, 1, 2]},
    // Equivalent forms which were never affected, included to pin down the difference.
    {filter: {val: {$in: ["Apple"]}}, expected: [0, 1, 2]},
    {filter: {$expr: {$eq: ["$val", "Apple"]}}, expected: [0, 1, 2]},
    // A multi-element list where only the first element matches under the collation.
    {filter: {$expr: {$in: ["$val", ["aPPle", "banana"]]}}, expected: [0, 1, 2]},
    // A list whose elements match nothing under any collation.
    {filter: {$expr: {$in: ["$val", ["banana"]]}}, expected: []},
    // The negation must exclude everything the positive form matches.
    {filter: {$expr: {$not: {$in: ["$val", ["Apple"]]}}}, expected: []},
    // Elements that are not eligible for the $in rewrite still need the collation.
    {filter: {$expr: {$in: ["$val", ["Apple", null]]}}, expected: [0, 1, 2]},
];

function idsOf(cursor) {
    return cursor
        .toArray()
        .map((doc) => doc._id)
        .sort();
}

/**
 * Runs every test case against 'coll'. 'collation' is passed with each query, or null when the
 * collection default collation should apply.
 */
function runTestCases(coll, collation) {
    for (const {filter, expected} of testCases) {
        const findCursor = collation ? coll.find(filter).collation(collation) : coll.find(filter);
        assert.eq(idsOf(findCursor), expected, "find with $expr", {filter, collation});

        const aggOptions = collation ? {collation} : {};
        const aggCursor = coll.aggregate([{$match: filter}], aggOptions);
        assert.eq(idsOf(aggCursor), expected, "aggregate $match with $expr", {filter, collation});

        assert.eq(
            expected.length,
            coll.countDocuments(filter, aggOptions),
            "countDocuments with $expr",
            {filter, collation},
        );
    }
}

describe("$expr + $in with a case-insensitive collation", function () {
    describe("collation supplied with the query", function () {
        const coll = db.expr_in_collation_query;

        before(function () {
            coll.drop();
            assert.commandWorked(coll.insert(docs));
        });

        after(function () {
            coll.drop();
        });

        it("matches all cases in a collection scan", function () {
            runTestCases(coll, caseInsensitive);
        });
    });

    describe("collation inherited from the collection default", function () {
        const coll = db.expr_in_collation_default;

        before(function () {
            coll.drop();
            assert.commandWorked(db.createCollection(coll.getName(), {collation: caseInsensitive}));
            assert.commandWorked(coll.insert(docs));
        });

        after(function () {
            coll.drop();
        });

        it("matches all cases in a collection scan", function () {
            runTestCases(coll, null);
        });
    });
});
