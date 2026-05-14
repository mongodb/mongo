/**
 * Tests that $match predicate pushdown into the time-series unpacking stage does not incorrectly
 * exclude a bucket whose measurement field contains NaN.
 *
 * Repro for SERVER-126494: a bucket-level filter computed from a measurement's min/max can drop a
 * bucket whose other measurements would satisfy the predicate when NaN participates in the
 * bucket's min/max summary. The time-series result must agree with the equivalent query against a
 * regular collection containing the same documents.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_62,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const timeField = "ts";
const metaField = "m";
const measureField = "v";

const tsColl = db[jsTestName() + "_ts"];
const regularColl = db[jsTestName() + "_regular"];

const setUp = function (docs) {
    tsColl.drop();
    regularColl.drop();
    assert.commandWorked(db.createCollection(tsColl.getName(), {timeseries: {timeField, metaField}}));
    assert.commandWorked(db.createCollection(regularColl.getName()));
    assert.commandWorked(tsColl.insertMany(docs));
    assert.commandWorked(regularColl.insertMany(docs));
};

/**
 * Runs 'pipeline' against both the time-series collection and a regular collection seeded with the
 * same documents. Asserts the two results agree as unordered sets.
 */
const assertTsMatchesRegular = function ({docs, pipeline, message}) {
    setUp(docs);
    const expected = regularColl.aggregate(pipeline).toArray();
    const actual = tsColl.aggregate(pipeline).toArray();
    assertArrayEq({
        actual,
        expected,
        extraErrorMsg: `${message}\npipeline=${tojson(pipeline)}\nexpected=${tojson(expected)}\nactual=${tojson(actual)}`,
    });
};

// Core SERVER-126494 repro: $lt predicate on a field whose bucket contains a NaN measurement.
// NaN compares unordered against any number, so a low-value measurement (v: 1) in the same bucket
// must still be returned. The bug: the bucket is dropped wholesale.
(function nanWithLessThanThenInclusionProject() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
        ],
        pipeline: [{$match: {v: {$lt: 50}}}, {$project: {_id: 0, v: 1}}],
        message: "$lt predicate must not drop bucket containing NaN sibling measurement",
    });
})();

// $gt predicate: by the same reasoning, the v: 100 measurement must survive even when v: NaN sits
// in the same bucket and would corrupt a naive max-based bucket-level filter.
(function nanWithGreaterThanThenInclusionProject() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
        ],
        pipeline: [{$match: {v: {$gt: 50}}}, {$project: {_id: 0, v: 1}}],
        message: "$gt predicate must not drop bucket containing NaN sibling measurement",
    });
})();

// $gte and $lte at the NaN boundary: NaN is not >= or <= any number, so only the numeric
// measurements should be returned. The bucket itself must still be unpacked.
(function nanWithRangePredicate() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
        ],
        pipeline: [{$match: {v: {$gte: 0, $lte: 200}}}, {$project: {_id: 0, v: 1}}],
        message: "Bounded range predicate must not drop bucket containing NaN sibling measurement",
    });
})();

// $eq: NaN: a query for NaN equality should still surface the NaN measurement and must not be
// short-circuited by min/max-driven bucket pruning.
(function matchOnNanItself() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
        ],
        pipeline: [{$match: {v: {$eq: NaN}}}, {$project: {_id: 0, v: 1}}],
        message: "$eq: NaN must surface the NaN measurement",
    });
})();

// $match without a follow-on $project: ensures the bug is not specific to the inclusion-project
// shape. Predicate pushdown must agree with the regular collection regardless of subsequent
// projection.
(function nanWithLessThanNoProject() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
        ],
        pipeline: [{$match: {v: {$lt: 50}}}],
        message: "$lt predicate without follow-on $project must agree with regular collection",
    });
})();

// Multiple buckets, only one contaminated with NaN: the clean bucket and the surviving numeric
// measurements in the NaN bucket must both be returned. Different metaField values force separate
// buckets.
(function nanInOneBucketAcrossManyBuckets() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: 1},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:02:00Z"), m: 1, v: 100},
            {ts: ISODate("2024-01-01T00:03:00Z"), m: 2, v: 2},
            {ts: ISODate("2024-01-01T00:04:00Z"), m: 2, v: 42},
            {ts: ISODate("2024-01-01T00:05:00Z"), m: 2, v: 1000},
        ],
        pipeline: [{$match: {v: {$lt: 50}}}, {$project: {_id: 0, v: 1, m: 1}}],
        message: "Multi-bucket scenario: NaN in one bucket must not affect the other",
    });
})();

// All-NaN bucket: every measurement is NaN. No predicate on numeric ranges should return a
// document, but the query must not error and must agree with the regular collection.
(function allNanBucket() {
    assertTsMatchesRegular({
        docs: [
            {ts: ISODate("2024-01-01T00:00:00Z"), m: 1, v: NaN},
            {ts: ISODate("2024-01-01T00:01:00Z"), m: 1, v: NaN},
        ],
        pipeline: [{$match: {v: {$lt: 50}}}, {$project: {_id: 0, v: 1}}],
        message: "All-NaN bucket: numeric range predicate returns no documents",
    });
})();
