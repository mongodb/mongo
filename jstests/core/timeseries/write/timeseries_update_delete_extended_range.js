/**
 * This test exercises updates and deletes on time series collections with an without extended range
 * data, and verifies results and that predicates are generated correctly:
 *   - No _id predicates if the collection contains extended range values.
 *   - In collections with no extended range values, if a predicate compares the time column with an
 *     extended range date, we produce either a trivially true bucket filter or '{$alwaysFalse: 1}'
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {
    makeBucketFilter,
    metaFieldName,
    testFindOneAndRemove,
    testFindOneAndUpdate,
    timeFieldName
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

{
    // A set of documents that contain extended range data before and after the epoch.
    const extendedRangeDocs = [
        {
            [timeFieldName]: ISODate("1968-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 0,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 1,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 2,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("2040-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 3,
            a: 10,
        },
    ];

    // A set of documents that does not contain extended range data.
    const normalDocs = [
        {
            [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 0,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 1,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 2,
            a: 10,
        },
        {
            [timeFieldName]: ISODate("2038-01-01T00:00:00Z"),
            [metaFieldName]: 1,
            _id: 3,
            a: 10,
        },
    ];

    (function testFindOneAndUpdate_extendedRange() {
        testFindOneAndUpdate({
            initialDocList: extendedRangeDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
                update: {$set: {a: 20}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1968-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 0,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 1,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 2,
                        a: 20,
                    },
                    {
                        [timeFieldName]: ISODate("2040-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                returnDoc: {
                    [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 2,
                    a: 10,
                },
                bucketFilter: makeBucketFilter({
                    "$and": [
                        // No _id predicate due to extended range data in the collection.
                        {"control.max.time": {"$_internalExprGt": ISODate("1972-01-01T00:00:00Z")}},
                        {"control.min.time": {"$_internalExprGt": ISODate("1971-12-31T23:00:00Z")}}
                    ]
                }),
                residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nModified: 1
            },
        });
    })();

    (function testFindOneAndRemove_extendedRange() {
        testFindOneAndRemove({
            initialDocList: extendedRangeDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1968-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 0,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 1,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2040-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                deletedDoc: {
                    [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 2,
                    a: 10,
                },
                bucketFilter: makeBucketFilter({
                    "$and": [
                        // No _id predicate due to extended range data in the collection.
                        {"control.max.time": {"$_internalExprGt": ISODate("1972-01-01T00:00:00Z")}},
                        {"control.min.time": {"$_internalExprGt": ISODate("1971-12-31T23:00:00Z")}}
                    ]
                }),
                residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nDeleted: 1
            },
        });
    })();

    (function testFindOneAndUpdate_normal() {
        testFindOneAndUpdate({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
                update: {$set: {a: 20}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 0,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 1,
                        a: 20,
                    },
                    {
                        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 2,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2038-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                returnDoc: {
                    [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 1,
                    a: 10,
                },
                bucketFilter: makeBucketFilter({
                    "$and": [
                        // This _id predicate is allowed because there is no extended range data.
                        {"_id": {"$gt": ObjectId("03c258f0ffffffffffffffff")}},
                        {"control.max.time": {"$_internalExprGt": ISODate("1972-01-01T00:00:00Z")}},
                        {"control.min.time": {"$_internalExprGt": ISODate("1971-12-31T23:00:00Z")}},
                    ]
                }),
                residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nModified: 1
            },
        });
    })();

    (function testFindOneAndRemove_normal() {
        testFindOneAndRemove({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 0,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 2,
                        a: 20,
                    },
                    {
                        [timeFieldName]: ISODate("2039-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                deletedDoc: {
                    [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 1,
                    a: 10,
                },
                bucketFilter: makeBucketFilter({
                    "$and": [
                        // This _id predicate is allowed because there is no extended range data.
                        {"_id": {"$gt": ObjectId("03c258f0ffffffffffffffff")}},
                        {"control.max.time": {"$_internalExprGt": ISODate("1972-01-01T00:00:00Z")}},
                        {"control.min.time": {"$_internalExprGt": ISODate("1971-12-31T23:00:00Z")}},
                    ]
                }),
                residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nDeleted: 1
            },
        });
    })();

    (function testFindOneAndUpdate_normalExtendedPredicateAll() {
        testFindOneAndUpdate({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1950-01-01T00:00:00Z")}},
                update: {$set: {a: 20}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 0,
                        a: 20,
                    },
                    {
                        [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 1,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 2,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2038-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                returnDoc: {
                    [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 0,
                    a: 10,
                },
                // The bucket filter is trivially true (empty predicate).
                // All the documents in the collection are after 1950.
                bucketFilter: makeBucketFilter({}),
                residualFilter: {"time": {"$gt": ISODate("1950-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nModified: 1
            },
        });
    })();

    (function testFindOneAndRemove_normalExtendedPredicateAll() {
        testFindOneAndRemove({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$gt: ISODate("1950-01-01T00:00:00Z")}},
            },
            res: {
                resultDocList: [
                    {
                        [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 1,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 2,
                        a: 10,
                    },
                    {
                        [timeFieldName]: ISODate("2039-01-01T00:00:00Z"),
                        [metaFieldName]: 1,
                        _id: 3,
                        a: 10,
                    },
                ],
                deletedDoc: {
                    [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 0,
                    a: 10,
                },
                // The bucket filter is trivially true (empty predicate).
                // All the documents in the collection are after 1950.
                bucketFilter: makeBucketFilter({}),
                residualFilter: {"time": {"$gt": ISODate("1950-01-01T00:00:00Z")}},
                nBucketsUnpacked: 1,
                nDeleted: 1
            },
        });
    })();

    (function testFindOneAndUpdate_normalExtendedPredicateNone() {
        testFindOneAndUpdate({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$lt: ISODate("1950-01-01T00:00:00Z")}},
                update: {$set: {a: 20}},
            },
            res: {
                resultDocList: normalDocs,
                returnDoc: null,
                // None of the documents in the collection can be before 1950.
                bucketFilter: makeBucketFilter({$alwaysFalse: 1}),
                residualFilter: {"time": {"$lt": ISODate("1950-01-01T00:00:00Z")}},
                nBucketsUnpacked: 0,
                nModified: 0
            },
        });
    })();

    (function testFindOneAndRemove_normalExtendedPredicateNone() {
        testFindOneAndRemove({
            initialDocList: normalDocs,
            cmd: {
                filter: {[timeFieldName]: {$lt: ISODate("1950-01-01T00:00:00Z")}},
            },
            res: {
                resultDocList: normalDocs,
                deletedDoc: null,
                // None of the documents in the collection can be before 1950.
                bucketFilter: makeBucketFilter({$alwaysFalse: 1}),
                residualFilter: {"time": {"$lt": ISODate("1950-01-01T00:00:00Z")}},
                nBucketsUnpacked: 0,
                nDeleted: 0
            },
        });
    })();
}
