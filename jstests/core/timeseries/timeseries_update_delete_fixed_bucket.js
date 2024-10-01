/**
 * Test the fixed bucketing optimization on update and delete commands on time-series collection.
 * The optimization produces tighter bounds on the bucket level predicates if there is a filter on
 * the timeField, and removes the 'residualFilter' if the predicate uses $lt or $gte.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   featureFlagTSBucketingParametersUnchanged,
 *  # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {
    makeBucketFilter,
    metaFieldName,
    testDeleteOne,
    testFindOneAndRemove,
    testFindOneAndUpdate,
    testUpdateOne,
    timeFieldName
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

{
    const roundingParam = 900;  // number of seconds in 15 minutes.
    const tsOptions = {bucketMaxSpanSeconds: roundingParam, bucketRoundingSeconds: roundingParam};
    const startingTime = ISODate("2023-02-06T01:30:00Z");
    const offset = roundingParam * 1000;
    // Different time values stored in ascending order.
    const times = [
        new Date(startingTime.getTime() - offset / 2),
        new Date(startingTime.getTime()),
        new Date(startingTime.getTime() + offset / 2),
        new Date(startingTime.getTime() + offset),
    ];
    // Documents that will span across multiple buckets.
    const doc_a_early_time = {[timeFieldName]: times[0], [metaFieldName]: 1, _id: 1, a: 2, b: 1};
    const doc_b_start_time = {[timeFieldName]: times[1], [metaFieldName]: 1, _id: 1, b: 1};
    const doc_a_late_time = {[timeFieldName]: times[2], [metaFieldName]: 1, _id: 1, b: 1, a: 3};
    const doc_a_latest_time = {[timeFieldName]: times[3], [metaFieldName]: 1, _id: 1, b: 1, a: 5};

    const doc_b_early_time = {[timeFieldName]: times[0], [metaFieldName]: 1, _id: 1, b: 1};
    const doc_a_late_time_m2 = {[timeFieldName]: times[2], [metaFieldName]: 2, _id: 1, b: 1, a: 3};

    /**
     * The following tests confirm that the predicates generated from the fixed bucket rewrites
     * return the expected results for update commands.
     */
    // $lt will remove the residualFilter, since the predicate aligns with the bucket boundaries.
    (function testUpdateOneUnset_NoFilter() {
        testFindOneAndUpdate({
            initialDocList: [doc_a_early_time, doc_b_start_time, doc_a_late_time],
            cmd: {
                filter: {[timeFieldName]: {$lt: startingTime}},
                update: {$unset: {a: ""}},
            },
            res: {
                resultDocList: [doc_b_early_time, doc_b_start_time, doc_a_late_time],
                returnDoc: doc_a_early_time,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$lt: ObjectId("63e058180000000000000000")}},
                        {"control.max.time": {$_internalExprLt: ISODate("2023-02-06T01:45:00Z")}},
                        {"control.min.time": {$_internalExprLt: startingTime}},
                    ]
                }),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $gt will always have a residualFilter.
    (function testUpdateOnePipelineUpdate_Filter() {
        testFindOneAndUpdate({
            initialDocList: [doc_a_early_time, doc_b_start_time, doc_a_late_time],
            cmd: {
                filter: {[timeFieldName]: {$gt: startingTime}},
                update: [{$set: {[metaFieldName]: 2}}],
            },
            res: {
                resultDocList: [doc_a_early_time, doc_b_start_time, doc_a_late_time_m2],
                returnDoc: doc_a_late_time,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$gt: ObjectId("63e05494ffffffffffffffff")}},
                        {"control.max.time": {$_internalExprGt: startingTime}},
                        {"control.min.time": {$_internalExprGte: startingTime}},
                    ]
                }),
                residualFilter: {"time": {"$gt": startingTime}},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $gte will remove the residualFilter, since the predicate aligns with the bucket boundaries.
    (function testUpdateOneUpsert_NoFilter() {
        testFindOneAndUpdate({
            initialDocList: [doc_b_start_time, doc_a_late_time],
            cmd: {
                filter: {[timeFieldName]: {$gte: times[3]}},
                update: doc_a_latest_time,
                returnNew: true,
                upsert: true
            },
            res: {
                resultDocList: [doc_b_start_time, doc_a_late_time, doc_a_latest_time],
                returnDoc: doc_a_latest_time,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$gte: ObjectId("63e058180000000000000000")}},
                        {"control.max.time": {$_internalExprGte: times[3]}},
                        {"control.min.time": {$_internalExprGte: times[3]}},
                    ]
                }),
                residualFilter: {},
                nBucketsUnpacked: 0,
                nMatched: 0,
                nModified: 0,
                nUpserted: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $lt will remove the residualFilter, since the predicate aligns with the bucket boundaries.
    (function testUpdateOne_MatchSomeDocs() {
        testUpdateOne({
            initialDocList: [doc_a_early_time, doc_b_start_time],
            updateQuery: {[timeFieldName]: {$lt: times[3]}},
            updateObj: {$set: {[metaFieldName]: 2}},
            // Don't validate exact results as we could update any doc.
            nMatched: 1,
            timeseriesOptions: tsOptions
        });
    })();

    // $lt and $gte will remove the residualFilter with an $and predicate that align with the bucket
    // boundaries.
    (function testUpdateOne_ConjunctionPredicate() {
        const query = {
            $and: [{[timeFieldName]: {$lt: times[3]}}, {[timeFieldName]: {$gte: times[1]}}]
        };
        testFindOneAndUpdate({
            initialDocList: [doc_a_early_time, doc_b_start_time],
            cmd: {filter: query, update: {$set: {[metaFieldName]: 2}}},
            res: {
                resultDocList: [
                    doc_a_early_time,
                    {[timeFieldName]: times[1], [metaFieldName]: 2, _id: 1, b: 1},
                ],
                returnDoc: doc_b_start_time,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$lt: ObjectId("63e05b9c0000000000000000")}},
                        {_id: {$gte: ObjectId("63e054940000000000000000")}},
                        {"control.max.time": {$_internalExprGte: times[1]}},
                        {"control.min.time": {$_internalExprGte: times[1]}},
                        {
                            "control.max.time":
                                {$_internalExprLt: new Date(times[3].getTime() + offset)}
                        },
                        {"control.min.time": {$_internalExprLt: times[3]}},
                    ]
                }),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $gte and a predicate on the metaField will remove the residualFilter.
    (function testUpdateOne_MetaPredicateIncluded() {
        const query = {[timeFieldName]: {$gte: startingTime}, [metaFieldName]: {$eq: 1}};
        testFindOneAndUpdate({
            initialDocList: [doc_a_early_time, doc_a_latest_time],
            cmd: {filter: query, update: {$set: {a: 10}}},
            res: {
                resultDocList: [
                    doc_a_early_time,
                    {[timeFieldName]: times[3], [metaFieldName]: 1, _id: 1, b: 1, a: 10}
                ],
                returnDoc: doc_a_latest_time,
                bucketFilter: makeBucketFilter({meta: {$eq: 1}}, {
                    $and: [
                        {_id: {$gte: ObjectId("63e054940000000000000000")}},
                        {"control.max.time": {$_internalExprGte: ISODate("2023-02-06T01:30:00Z")}},
                        {"control.min.time": {$_internalExprGte: ISODate("2023-02-06T01:30:00Z")}},
                    ]
                }),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nMatched: 1,
                nModified: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    /**
     * The following tests confirm that the predicates generated from the fixed bucket rewrites
     * return the expected results for delete commands.
     */
    (function testDeleteOne_MatchMultipleBuckets() {
        testDeleteOne({
            initialDocList:
                [doc_a_early_time, doc_b_start_time, doc_a_late_time, doc_a_latest_time],
            filter: {[timeFieldName]: {$lte: times[3]}},
            // Don't validate exact results as we could delete any doc.
            nDeleted: 1,
        });
    })();

    (function testDeleteOne_MatchOneDoc_InBucket() {
        testDeleteOne({
            initialDocList:
                [doc_a_early_time, doc_b_start_time, doc_a_late_time, doc_a_latest_time],
            filter: {[timeFieldName]: {$lte: times[0]}},
            expectedDocList: [doc_b_start_time, doc_a_late_time, doc_a_latest_time],
            nDeleted: 1,
        });
    })();

    // $lt will remove the residualFilter, since the predicate aligns with the bucket boundaries.
    // Here the bucket boundaries do not align, so we expect a residualFilter.
    (function testFindOneAndRemove_Filter() {
        testFindOneAndRemove({
            initialDocList: [doc_b_start_time, doc_a_late_time, doc_a_latest_time],
            cmd: {filter: {[timeFieldName]: {$lt: times[2]}}},
            res: {
                expectedDocList: [doc_a_late_time, doc_a_latest_time],
                nDeleted: 1,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$lt: ObjectId("63e059da0000000000000000")}},
                        {
                            "control.max.time":
                                {$_internalExprLt: new Date(times[2].getTime() + offset)}
                        },
                        {"control.min.time": {$_internalExprLt: times[2]}},
                    ]
                }),
                residualFilter: {[timeFieldName]: {$lt: times[2]}},
                nBucketsUnpacked: 1,
                nReturned: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $eq will always have a residual filter.
    (function testFindOneAndRemove_Filter_EQ() {
        testFindOneAndRemove({
            initialDocList: [doc_b_start_time, doc_a_late_time],
            cmd: {filter: {[timeFieldName]: times[2]}},
            res: {
                expectedDocList: [doc_b_start_time],
                nDeleted: 1,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$lte: ObjectId("63e059daffffffffffffffff")}},
                        {_id: {$gte: ObjectId("63e056560000000000000000")}},
                        {"control.max.time": {$_internalExprGte: ISODate("2023-02-06T01:37:30Z")}},
                        {"control.min.time": {$_internalExprGte: ISODate("2023-02-06T01:30:00Z")}},
                        {"control.max.time": {$_internalExprLte: ISODate("2023-02-06T01:52:30Z")}},
                        {"control.min.time": {$_internalExprLte: ISODate("2023-02-06T01:37:30Z")}},
                    ]
                }),
                residualFilter: {[timeFieldName]: {$eq: times[2]}},
                nBucketsUnpacked: 1,
                nReturned: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();

    // $gte will remove the residualFilter, since the predicate aligns with the bucket boundaries.
    (function testFindOneAndRemove_NoFilter() {
        testFindOneAndRemove({
            initialDocList: [doc_b_start_time, doc_a_late_time, doc_a_latest_time],
            cmd: {filter: {[timeFieldName]: {$gte: times[3]}}},
            res: {
                expectedDocList: [doc_b_start_time, doc_a_late_time],
                nDeleted: 1,
                bucketFilter: makeBucketFilter({
                    $and: [
                        {_id: {$gte: ObjectId("63e058180000000000000000")}},
                        {"control.max.time": {$_internalExprGte: times[3]}},
                        {"control.min.time": {$_internalExprGte: times[3]}},
                    ]
                }),
                residualFilter: {},
                nBucketsUnpacked: 1,
                nReturned: 1,
            },
            timeseriesOptions: tsOptions
        });
    })();
}
