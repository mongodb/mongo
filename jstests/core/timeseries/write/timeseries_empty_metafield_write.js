/**
 * Tests updates and deletes on a time-series collection whose metaField is the empty string ("").
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   does_not_support_stepdowns,
 *   # Explaining an update whose paths are all named "" only parses on a node that knows the
 *   # collection is time-series (so the empty metaField is translated to "meta"). A shard that
 *   # does not own the collection rejects the update with EmptyFieldName.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const timeField = "t";
const metaField = "";
const baseTime = ISODate("2024-01-01T00:00:00Z");
const docs = [
    {[timeField]: new Date(baseTime.getTime() + 0), [metaField]: 1, _id: 1, value: 10},
    {[timeField]: new Date(baseTime.getTime() + 1000), [metaField]: 1, _id: 2, value: 20},
    {[timeField]: new Date(baseTime.getTime() + 2000), [metaField]: 2, _id: 3, value: 30},
    {[timeField]: new Date(baseTime.getTime() + 3000), [metaField]: 2, _id: 4, value: 40},
];

describe("time-series writes on an empty-string metaField", function () {
    before(function () {
        this.coll = db.getCollection(jsTestName());
    });

    beforeEach(function () {
        this.coll.drop();
        assert.commandWorked(
            db.createCollection(this.coll.getName(), {
                timeseries: {timeField: timeField, metaField: metaField},
            }),
            "creating a time-series collection with an empty-string metaField should be allowed",
        );
        assert.commandWorked(this.coll.insertMany(docs));
    });

    it("updateMany on the metaField", function () {
        const res = assert.commandWorked(
            this.coll.updateMany({[metaField]: 1}, {$set: {[metaField]: 3}}),
        );
        assert.eq(2, res.modifiedCount, "unexpected update result", {res});

        assert.eq(0, this.coll.find({[metaField]: 1}).itcount());
        assert.sameMembers(
            [1, 2],
            this.coll
                .find({[metaField]: 3})
                .toArray()
                .map((d) => d._id),
        );
    });

    it("deleteMany on the metaField fails gracefully", function () {
        assert.throwsWithCode(() => this.coll.deleteMany({[metaField]: 2}), 40352);
        assert.eq(docs.length, this.coll.find().itcount());
    });

    it("deleteMany with an empty query fails gracefully", function () {
        assert.throwsWithCode(() => this.coll.deleteMany({}), 40352);
        assert.eq(docs.length, this.coll.find().itcount());
    });

    it("partial bucket delete on a measurement field fails gracefully", function () {
        assert.throwsWithCode(() => this.coll.deleteMany({value: 40}), 40352);
        assert.eq(docs.length, this.coll.find().itcount());
    });
});
