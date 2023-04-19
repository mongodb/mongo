/**
 * Tests running the delete command on a time-series collection while specifying query-level and/or
 * collection-level collation.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   requires_fcv_71,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");     // For getPlanStage().
load("jstests/libs/fixture_helpers.js");  // For 'isMongos'

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const collNamePrefix = "timeseries_delete_collation_";
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
 * Confirms that a set of deletes returns the expected set of documents and runs the correct delete
 * stage and bucket query.
 */
function runTest({
    deleteFilter,
    queryCollation,
    collectionCollation,
    nDeleted,
    expectedBucketQuery,
    expectedDeleteStage
}) {
    jsTestLog(`Running ${tojson(deleteFilter)} with queryCollation: ${
        tojson(queryCollation)} and collectionCollation: ${tojson(collectionCollation)}`);

    assert(expectedDeleteStage === "TS_MODIFY" || expectedDeleteStage === "BATCHED_DELETE");

    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        collation: collectionCollation
    }));
    assert.commandWorked(coll.insert(docs));

    const deleteCommand = {
        delete: coll.getName(),
        deletes: [{q: deleteFilter, limit: 0, collation: queryCollation}]
    };
    const explain = testDB.runCommand({explain: deleteCommand, verbosity: "queryPlanner"});
    const parsedQuery = FixtureHelpers.isMongos(testDB)
        ? explain.queryPlanner.winningPlan.shards[0].parsedQuery
        : explain.queryPlanner.parsedQuery;

    assert.eq(expectedBucketQuery, parsedQuery, `Got wrong parsedQuery: ${tojson(explain)}`);
    assert.neq(null,
               getPlanStage(explain.queryPlanner.winningPlan, expectedDeleteStage),
               `${expectedDeleteStage} stage not found in the plan: ${tojson(explain)}`);

    const res = assert.commandWorked(testDB.runCommand(deleteCommand));
    assert.eq(nDeleted, res.n);
}

(function testNoCollation() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {str: "hello"},
        nDeleted: 3,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "hello"}},
                {"control.min.str": {$_internalExprLte: "hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "A"},
        nDeleted: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
})();

(function testQueryLevelCollation() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseSensitive,
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        nDeleted: 6,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseSensitive,
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        nDeleted: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
})();

(function testCollectionLevelCollation() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        collectionCollation: caseSensitive,
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {str: "Hello"},
        collectionCollation: caseInsensitive,
        nDeleted: 6,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        collectionCollation: caseSensitive,
        nDeleted: 0,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        collectionCollation: caseInsensitive,
        nDeleted: 4,
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
})();

(function testQueryLevelCollationOverridesDefault() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nDeleted: 6,
        expectedBucketQuery: {
            $and: [
                closedBucketFilter,
                {"control.max.str": {$_internalExprGte: "Hello"}},
                {"control.min.str": {$_internalExprLte: "Hello"}}
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nDeleted: 6,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "A", str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nDeleted: 2,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nDeleted: 4,
        // We can push down bucket filter for BATCHED_DELETE stage with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nDeleted: 4,
        // We can push down bucket filter for BATCHED_DELETE stage with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
})();

(function testQueryLevelSimpleCollationOverridesNonSimpleDefault() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 0,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {str: "hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 3,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: closedBucketFilter,
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "a", str: "hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 0,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "A", str: "HELLO"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 1,
        // We cannot push down bucket metric predicate for TS_MODIFY stage when the query level
        // collation overrides the collection level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "TS_MODIFY"
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 0,
        // We can push down bucket filter for BATCHED_DELETE stage with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "a"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
    runTest({
        deleteFilter: {[metaFieldName]: "A"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 4,
        // We can push down bucket filter for BATCHED_DELETE stage with the query level collation.
        expectedBucketQuery: {
            $and: [
                {meta: {$eq: "A"}},
                closedBucketFilter,
            ]
        },
        expectedDeleteStage: "BATCHED_DELETE"
    });
})();
})();
