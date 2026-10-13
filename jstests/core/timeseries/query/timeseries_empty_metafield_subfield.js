/**
 * Tests predicates on a subfield of an empty-string metaField ("") on a time-series collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

const timeField = "t";
const metaField = "";
const baseTime = ISODate("2024-01-01T00:00:00Z");
const at = (offsetSeconds) => new Date(baseTime.getTime() + offsetSeconds * 1000);

const docs = [
    {_id: 1, [timeField]: at(0), [metaField]: {a: 1, b: "x"}, value: 10},
    {_id: 2, [timeField]: at(1), [metaField]: {a: 2, b: "x"}, value: 20},
    {_id: 3, [timeField]: at(2), [metaField]: {a: 2, b: "y"}, value: 30},
    {_id: 4, [timeField]: at(3), [metaField]: {a: 1, b: "y"}, value: 40},
    // The meta value itself contains an empty-named subfield, reachable via '..a'.
    {_id: 5, [timeField]: at(4), [metaField]: {"": {a: 7}}, value: 50},
];

describe("time-series predicates on a subfield of an empty-string metaField", function () {
    let coll;

    before(function () {
        coll = db.getCollection(jsTestName());
        coll.drop();
        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
            "creating a time-series collection with an empty-string metaField should be allowed",
        );
        assert.commandWorked(coll.insertMany(docs));
    });

    // Asserts that both find() and an equivalent $match return exactly the documents whose _id is
    // in 'expectedIds'. Checking both entry points guards against the predicate being translated on
    // only one of them.
    function assertMatchesIds(query, expectedIds) {
        const expected = docs.filter((d) => expectedIds.includes(d._id));
        assert.sameMembers(expected, coll.find(query).toArray(), {query, expectedIds});
        assert.sameMembers(expected, coll.aggregate([{$match: query}]).toArray(), {
            query,
            expectedIds,
        });
    }

    describe("subfield predicates (the empty metaField component maps onto 'meta')", function () {
        it("equality on a meta subfield", function () {
            assertMatchesIds({".a": 1}, [1, 4]);
        });

        it("$in on a meta subfield", function () {
            assertMatchesIds({".a": {$in: [1, 99]}}, [1, 4]);
        });

        it("range on a meta subfield", function () {
            assertMatchesIds({".a": {$gt: 1}}, [2, 3]);
        });

        it("$exists on a meta subfield", function () {
            // _id 5 has '..a', not '.a', so it must not match.
            assertMatchesIds({".a": {$exists: true}}, [1, 2, 3, 4]);
        });

        it("negated predicate on a meta subfield", function () {
            assertMatchesIds({".a": {$ne: 1}}, [2, 3, 5]);
        });

        it("predicate on a different meta subfield", function () {
            assertMatchesIds({".b": "y"}, [3, 4]);
        });

        it("meta subfield conjoined with a measurement predicate", function () {
            assertMatchesIds({".a": 1, value: {$gt: 20}}, [4]);
        });

        it("$or over a meta subfield and a measurement predicate", function () {
            assertMatchesIds({$or: [{".a": 1}, {value: 50}]}, [1, 4, 5]);
        });

        it("a path with several empty-named components", function () {
            assertMatchesIds({"..a": 7}, [5]);
        });

        it("exact match on the whole empty-named metaField", function () {
            assertMatchesIds({[metaField]: {a: 1, b: "x"}}, [1]);
        });
    });
});
