/**
 * Verifies that a value-form $elemMatch whose body optimizes away to empty $and still returns the
 * correct results on a timeseries collection that has been tracked after moveCollection.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_80,
 *   multiversion_incompatible,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("timeseries $elemMatch after moveCollection", function () {
    const dbName = "test";
    const timeField = "t";
    const metaField = "m";

    let st;
    let testDB;
    let tsColl;
    let controlColl;

    // '_id: 2' holds an array of scalars, which is the differentiating case: it matches only if the
    // value form of $elemMatch is preserved.
    const docs = [
        // 'array' is a scalar, so no form of $elemMatch matches.
        {
            _id: 1,
            [timeField]: ISODate("1970-01-01T00:00:00Z"),
            [metaField]: {m1: 1},
            array: 0,
            a: 0,
        },
        // 'array' is an array of scalars: only the value form matches.
        {
            _id: 2,
            [timeField]: ISODate("1970-01-02T00:00:00Z"),
            [metaField]: {m1: 2},
            array: [0],
            a: 0,
        },
        // 'array' is an array of arrays: both forms match, since an array is also a BSON object.
        {
            _id: 3,
            [timeField]: ISODate("1970-01-03T00:00:00Z"),
            [metaField]: {m1: 3},
            array: [[0]],
            a: 0,
        },
        // 'array' is an array of objects: both forms match.
        {
            _id: 4,
            [timeField]: ISODate("1970-01-04T00:00:00Z"),
            [metaField]: {m1: 4},
            array: [{a: 0}],
            a: 0,
        },
        // No 'array' field at all: no form of $elemMatch matches.
        {_id: 5, [timeField]: ISODate("1970-01-05T00:00:00Z"), [metaField]: {m1: 5}, a: 0},
    ];

    before(function () {
        st = new ShardingTest({shards: 2});
        testDB = st.s.getDB(dbName);
        tsColl = testDB.ts_elemmatch;
        controlColl = testDB.control_elemmatch;

        assert.commandWorked(
            testDB.createCollection(tsColl.getName(), {
                timeseries: {timeField: timeField, metaField: metaField},
            }),
        );
        assert.commandWorked(tsColl.insertMany(docs));
        assert.commandWorked(controlColl.insertMany(docs));

        jsTest.log("Move the timeseries collection, which also makes it tracked.");
        assert.commandWorked(
            st.s.adminCommand({
                moveCollection: `${dbName}.${tsColl.getName()}`,
                toShard: st.shard1.shardName,
            }),
        );
    });

    it("returns correct results for a value-form $elemMatch whose body is optimized away", function () {
        // The inner $elemMatch can never match because {$in: []} is unsatisfiable, so $not of it is
        // always true and the predicate reduces to "'array' is a non-empty array".
        const valueFormPipeline = [
            {
                $match: {
                    array: {
                        $elemMatch: {
                            $not: {$elemMatch: {a: {$in: []}, $and: [{a: {$eq: -14, $lte: 0}}]}},
                        },
                    },
                },
            },
            {$project: {_id: 1}},
            {$sort: {_id: 1}},
        ];

        assert.eq([{_id: 2}, {_id: 3}, {_id: 4}], tsColl.aggregate(valueFormPipeline).toArray());
        assert.eq(
            controlColl.aggregate(valueFormPipeline).toArray(),
            tsColl.aggregate(valueFormPipeline).toArray(),
            "timeseries and control collections disagree on the value-form $elemMatch",
        );
    });

    it("preserves object-form $elemMatch semantics", function () {
        // The object form must keep its own semantics: its body is also always-true after
        // optimization but the scalar element of '_id: 2' does not qualify.
        const objectFormPipeline = [
            // 'f' does not exist in any document, so the predicate must not be read as matching a
            // particular field: it only requires the element to be document-shaped.
            {$match: {array: {$elemMatch: {f: {$not: {$in: []}}}}}},
            {$project: {_id: 1}},
            {$sort: {_id: 1}},
        ];

        assert.eq([{_id: 3}, {_id: 4}], tsColl.aggregate(objectFormPipeline).toArray());
        assert.eq(
            controlColl.aggregate(objectFormPipeline).toArray(),
            tsColl.aggregate(objectFormPipeline).toArray(),
            "timeseries and control collections disagree on the object-form $elemMatch",
        );
    });

    it("treats an empty $elemMatch body as the object form", function () {
        // An empty body '{$elemMatch: {}}' selects the object form, so it matches elements that are
        // themselves objects or arrays but not scalar elements. This is the mirror image of the
        // value-form case above and guards against the object form ever being re-encoded.
        const emptyBodyPipeline = [
            {$match: {array: {$elemMatch: {}}}},
            {$project: {_id: 1}},
            {$sort: {_id: 1}},
        ];

        assert.eq([{_id: 3}, {_id: 4}], tsColl.aggregate(emptyBodyPipeline).toArray());
        assert.eq(
            controlColl.aggregate(emptyBodyPipeline).toArray(),
            tsColl.aggregate(emptyBodyPipeline).toArray(),
            "timeseries and control collections disagree on an empty-body $elemMatch",
        );
    });

    after(function () {
        st.stop();
    });
});
