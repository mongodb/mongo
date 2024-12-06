/**
 * Tests running the update command on a time-series collection while specifying query-level  and/or
 * collection-level collation.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Test explicitly relies on multi-updates.
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */

import {testCollation} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = "timeseries_update_collation_";
let testCaseId = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const caseSensitive = {
    locale: "en",
    strength: 3
};
const caseInsensitive = {
    locale: "en",
    strength: 2
};
const simple = {
    locale: "simple"
};
const closedBucketFilter = {
    "control.closed": {"$not": {"$eq": true}}
};

const docs = [
    {_id: 0, [timeFieldName]: dateTime, [metaFieldName]: "A", str: "HELLO"},
    {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "A", str: "hello"},
    {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: "A", str: "goodbye"},
    {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: "A"},
    {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: "B", str: "HELLO"},
    {_id: 5, [timeFieldName]: dateTime, [metaFieldName]: "B", str: "hello"},
    {_id: 6, [timeFieldName]: dateTime, [metaFieldName]: "B", str: "goodbye"},
    {_id: 7, [timeFieldName]: dateTime, [metaFieldName]: "B"},
    {_id: 8, [timeFieldName]: dateTime, str: "HELLO"},
    {_id: 9, [timeFieldName]: dateTime, str: "hello"},
    {_id: 10, [timeFieldName]: dateTime, str: "goodbye"},
    {_id: 11, [timeFieldName]: dateTime},
];

/**
 * Confirms that updates return the expected set of documents and run the correct bucket query.
 */
function runTest({
    updateFilter,
    queryCollation,
    collectionCollation,
    nModified,
    expectedBucketQuery,
}) {
    jsTestLog(`Running ${tojson(updateFilter)} with queryCollation: ${
        tojson(queryCollation)} and collectionCollation: ${tojson(collectionCollation)}`);

    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        collation: collectionCollation
    }));
    assert.commandWorked(coll.insert(docs));

    testCollation({
        testDB: testDB,
        coll: coll,
        filter: updateFilter,
        update: {$inc: {newField: 1}},
        queryCollation: queryCollation,
        nModified: nModified,
        expectedBucketQuery: expectedBucketQuery,
        expectedStage: "TS_MODIFY"
    });
}

(function testNoCollation() {
    // Residual filter.
    runTest({
        updateFilter: {str: "Hello"},
        nModified: 0,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
    });
    runTest({
        updateFilter: {str: "hello"},
        nModified: 3,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "hello"}},
                {"control.min.str": {$_internalExprLte: "hello"}}
            ]
        },
    });

    // Bucket filter.
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        nModified: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "A"},
        nModified: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
    });
})();

// Since the query collation does not match the collection collation, there should be no predicates
// on control fields. The predicates on meta field are okay because buckets are grouped based on
// real values, ignoring their collation.
(function testQueryLevelCollation() {
    // Residual filter.
    runTest({
        updateFilter: {str: "Hello"},
        queryCollation: caseSensitive,
        nModified: 0,
        expectedBucketQuery: closedBucketFilter,
    });
    runTest({
        updateFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        nModified: 6,
        expectedBucketQuery: closedBucketFilter,
    });

    // Bucket filter.
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        queryCollation: caseSensitive,
        nModified: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        nModified: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
})();

(function testCollectionLevelCollation() {
    // Residual filter.
    runTest({
        updateFilter: {str: "Hello"},
        collectionCollation: caseSensitive,
        nModified: 0,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
    });
    runTest({
        updateFilter: {str: "Hello"},
        collectionCollation: caseInsensitive,
        nModified: 6,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
    });

    // Bucket filter.
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        collectionCollation: caseSensitive,
        nModified: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        collectionCollation: caseInsensitive,
        nModified: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
})();

(function testQueryLevelCollationOverridesDefault() {
    // Residual filter.
    runTest({
        updateFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nModified: 6,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
    });
    runTest({
        updateFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nModified: 6,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
    });
    runTest({
        updateFilter: {[metaFieldName]: "A", str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nModified: 2,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
    });

    // Bucket filter.
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nModified: 4,
        // We can push down bucket filter with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nModified: 4,
        // We can push down bucket filter with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
})();

(function testQueryLevelSimpleCollationOverridesNonSimpleDefault() {
    // Residual filter.
    runTest({
        updateFilter: {str: "Hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 0,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
    });
    runTest({
        updateFilter: {str: "hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 3,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
    });
    runTest({
        updateFilter: {[metaFieldName]: "a", str: "hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 0,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "A", str: "HELLO"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 1,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
    });

    // Bucket filter.
    runTest({
        updateFilter: {[metaFieldName]: "a"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 0,
        // We can push down bucket filter with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
    });
    runTest({
        updateFilter: {[metaFieldName]: "A"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nModified: 4,
        // We can push down bucket filter with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
    });
})();