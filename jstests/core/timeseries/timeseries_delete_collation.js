/**
 * Tests running the delete command on a time-series collection while specifying query-level and/or
 * collection-level collation.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesDeletesSupport,
 * ]
 */

(function() {
"use strict";

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
 * Confirms that a set of deletes returns the expected set of documents.
 */
function runTest({deleteFilter, queryCollation, collectionCollation, nDeleted}) {
    const coll = testDB.getCollection(collNamePrefix + testCaseId++);
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
        collation: collectionCollation
    }));
    assert.commandWorked(coll.insert(docs));

    const res = assert.commandWorked(coll.deleteMany(deleteFilter, {collation: queryCollation}));
    assert.eq(nDeleted, res.deletedCount);
}

(function testNoCollation() {
    // Residual filter.
    runTest({deleteFilter: {str: "Hello"}, nDeleted: 0});
    runTest({deleteFilter: {str: "hello"}, nDeleted: 3});

    // Bucket filter.
    runTest({deleteFilter: {[metaFieldName]: "a"}, nDeleted: 0});
    runTest({deleteFilter: {[metaFieldName]: "A"}, nDeleted: 4});
})();

(function testQueryLevelCollation() {
    // Residual filter.
    runTest({deleteFilter: {str: "Hello"}, queryCollation: caseSensitive, nDeleted: 0});
    runTest({deleteFilter: {str: "Hello"}, queryCollation: caseInsensitive, nDeleted: 6});

    // Bucket filter.
    runTest({deleteFilter: {[metaFieldName]: "a"}, queryCollation: caseSensitive, nDeleted: 0});
    runTest({deleteFilter: {[metaFieldName]: "a"}, queryCollation: caseInsensitive, nDeleted: 4});
})();

(function testCollectionLevelCollation() {
    // Residual filter.
    runTest({deleteFilter: {str: "Hello"}, collectionCollation: caseSensitive, nDeleted: 0});
    runTest({deleteFilter: {str: "Hello"}, collectionCollation: caseInsensitive, nDeleted: 6});

    // Bucket filter.
    runTest(
        {deleteFilter: {[metaFieldName]: "a"}, collectionCollation: caseSensitive, nDeleted: 0});
    runTest(
        {deleteFilter: {[metaFieldName]: "a"}, collectionCollation: caseInsensitive, nDeleted: 4});
})();

(function testQueryLevelCollationOverridesDefault() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nDeleted: 6
    });
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nDeleted: 6
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseInsensitive,
        nDeleted: 4
    });
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: caseInsensitive,
        collectionCollation: caseSensitive,
        nDeleted: 4
    });
})();

(function testQueryLevelSimpleCollationOverridesNonSimpleDefault() {
    // Residual filter.
    runTest({
        deleteFilter: {str: "Hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 0
    });
    runTest({
        deleteFilter: {str: "hello"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 3
    });

    // Bucket filter.
    runTest({
        deleteFilter: {[metaFieldName]: "a"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 0
    });
    runTest({
        deleteFilter: {[metaFieldName]: "A"},
        queryCollation: simple,
        collectionCollation: caseInsensitive,
        nDeleted: 4
    });
})();
})();
