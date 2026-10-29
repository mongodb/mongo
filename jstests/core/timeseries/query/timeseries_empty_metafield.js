/**
 * Tests queries on a time-series collection whose metaField is the empty string ("").
 *
 * An empty string metaField is permitted at collection creation, but empty field names work differently
 * with different stages and commands.
 *   - $match / find predicates work.
 *   - Any operation that resolves the field through a FieldPath will fail (sort key, projection path)
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
const docs = [
    {[timeField]: new Date(baseTime.getTime() + 0), [metaField]: 1, _id: 1, value: 10},
    {[timeField]: new Date(baseTime.getTime() + 1000), [metaField]: 1, _id: 2, value: 20},
    {[timeField]: new Date(baseTime.getTime() + 2000), [metaField]: 2, _id: 3, value: 30},
    {[timeField]: new Date(baseTime.getTime() + 3000), [metaField]: 2, _id: 4, value: 40},
];

describe("time-series queries on an empty-string metaField", function () {
    let coll;

    before(function () {
        coll = db.getCollection(jsTestName());
        coll.drop();

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
            "creating a time-series collection with an empty-string metaField should be allowed",
        );

        assert.commandWorked(coll.createIndex({[metaField]: 1}));
        assert.commandWorked(coll.insertMany(docs));
    });

    describe("predicates on the metaField (supported)", function () {
        it("find() equality on the metaField", function () {
            const expected = docs.filter((d) => d[metaField] === 1);
            const actual = coll.find({[metaField]: 1}).toArray();
            assert.sameMembers(expected, actual);
        });

        it("find() $in on the metaField", function () {
            const expected = docs.filter((d) => [1, 99].includes(d[metaField]));
            const actual = coll.find({[metaField]: {$in: [1, 99]}}).toArray();
            assert.sameMembers(expected, actual);
        });

        it("$match range on the metaField", function () {
            const expected = docs.filter((d) => d[metaField] >= 2);
            const actual = coll.aggregate([{$match: {[metaField]: {$gte: 2}}}]).toArray();
            assert.sameMembers(expected, actual);
        });

        it("$or over the metaField and a measurement field", function () {
            const expected = docs.filter((d) => d[metaField] === 1 || d.value === 40);
            const actual = coll
                .aggregate([{$match: {$or: [{[metaField]: 1}, {value: 40}]}}])
                .toArray();
            assert.sameMembers(expected, actual);
        });
    });

    // Operations that fail with an empty metaField. These also fail on non-timeseries collections.
    describe("field-path references to the metaField", function () {
        it("find().sort() on the metaField", function () {
            assert.throwsWithCode(
                () =>
                    coll
                        .find()
                        .sort({[metaField]: 1})
                        .toArray(),
                40352,
            );
        });

        it("$sort on the metaField", function () {
            assert.throwsWithCode(
                () => coll.aggregate([{$sort: {[metaField]: 1}}]).toArray(),
                40352,
            );
        });

        it("$project including the metaField", function () {
            assert.throwsWithCode(
                () => coll.aggregate([{$project: {[metaField]: 1}}]).toArray(),
                40352,
            );
        });

        it("$set on the metaField", function () {
            assert.throwsWithCode(
                () => coll.aggregate([{$set: {[metaField]: 5}}]).toArray(),
                40352,
            );
        });

        it("$group by the metaField", function () {
            assert.throwsWithCode(
                () =>
                    coll
                        .aggregate([{$group: {_id: "$" + metaField, mn: {$min: "$value"}}}])
                        .toArray(),
                16872,
            );
        });

        it("$addFields computed from the metaField", function () {
            assert.throwsWithCode(
                () => coll.aggregate([{$addFields: {computed: "$" + metaField}}]).toArray(),
                16872,
            );
        });

        it("last-point query grouping by the metaField", function () {
            assert.throwsWithCode(
                () =>
                    coll
                        .aggregate([
                            {$sort: {[timeField]: -1}},
                            {$group: {_id: "$" + metaField, lastValue: {$first: "$value"}}},
                        ])
                        .toArray(),
                16872,
            );
        });
    });

    describe("queries not referencing the metaField (supported)", function () {
        it("$match on a measurement field", function () {
            const expected = docs.filter((d) => d.value >= 30);
            const actual = coll.find({value: {$gte: 30}}).toArray();
            assert.sameMembers(expected, actual);
        });

        it("$match on the timeField", function () {
            const cutoff = new Date(baseTime.getTime() + 2000);
            const expected = docs.filter((d) => d[timeField] < cutoff);
            const actual = coll.find({[timeField]: {$lt: cutoff}}).toArray();
            assert.sameMembers(expected, actual);
        });

        it("$count (count-like rewrite)", function () {
            const actual = coll.aggregate([{$count: "n"}]).toArray();
            assert.eq([{n: docs.length}], actual);
        });

        it("$sort on the timeField (bounded sort)", function () {
            const expected = docs
                .slice()
                .sort((a, b) => a[timeField] - b[timeField])
                .map((d) => d.value);
            const actual = coll
                .aggregate([{$sort: {[timeField]: 1}}])
                .toArray()
                .map((d) => d.value);
            assert.eq(expected, actual);
        });

        it("$sort + $group on the timeField (streaming group)", function () {
            const actual = coll
                .aggregate([
                    {$sort: {[timeField]: 1}},
                    {$group: {_id: "$" + timeField, total: {$sum: "$value"}}},
                ])
                .toArray();
            // Each measurement has a distinct time, so there is one group per document.
            assert.eq(docs.length, actual.length);
        });
    });
});
