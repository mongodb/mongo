/**
 * Tests that Compound Wildcard Indexes can be added/removed/listed from index filters and that
 * Compound Wildcard Indexes objey index filters.
 *
 * @tags: [
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_70,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 * ]
 */

import {WildcardIndexHelpers} from "jstests/libs/wildcard_index_helpers.js";

/**
 * Utility function to find an index filter by keyPattern or index name in the given filterList.
 */
function findFilter(cwiFilter, filterList) {
    for (const filter of filterList) {
        if (bsonWoCompare(cwiFilter.query, filter.query) == 0) {
            if (filter.indexes.find(keyPattern =>
                                        bsonWoCompare(cwiFilter.keyPattern, keyPattern) == 0)) {
                return filter;
            }
            if (filter.indexes.find(indexName => cwiFilter.indexName == indexName)) {
                return filter;
            }
        }
    }

    return null;
}

/**
 * Utility function to list index filters.
 */
function getFilters(coll) {
    const res = assert.commandWorked(coll.runCommand('planCacheListFilters'));
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    return res.filters;
}

/**
 * Sets an index filter given a query shape then confirms that the expected index was used
 * to answer a query.
 */
function assertExpectedIndexAnswersQueryWithFilter(
    coll, filterQuery, filterIndexes, query, expectedIndexName) {
    // Clear existing cache filters.
    assert.commandWorked(coll.runCommand('planCacheClearFilters'), 'planCacheClearFilters failed');

    // Make sure that the filter is set correctly.
    assert.commandWorked(
        coll.runCommand('planCacheSetFilter', {query: filterQuery, indexes: filterIndexes}));
    assert.eq(1,
              getFilters(coll).length,
              'no change in query settings after successfully setting index filters');

    // Check that expectedIndex index was used over another index.
    const explain = assert.commandWorked(coll.find(query).explain('executionStats'));

    WildcardIndexHelpers.assertExpectedIndexIsUsed(explain, expectedIndexName);
}

const collectionName = "compound_wildcard_index_filter";
const coll = db[collectionName];
coll.drop();

const cwiFilterList = [
    // Note: 'wildcardProjection' cannot be specified if the wildcard field is not "$**".
    {
        keyPattern: {a: 1, b: 1, "c.$**": 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.d": 1},
        correspondingRegularKeyPattern: {a: 1, b: 1, "c.d": 1},
    },
    {
        keyPattern: {a: 1, "c.$**": -1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.a": 1},
        correspondingRegularKeyPattern: {a: 1, "c.a": -1, b: 1},
    },
    {
        keyPattern: {"c.$**": 1, a: 1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.front": 1},
        correspondingRegularKeyPattern: {"c.front": 1, a: 1, b: 1},
    },
    {
        keyPattern: {a: -1, b: 1, "$**": 1},
        wildcardProjection: {"c": 1},
        query: {a: 1, b: 1, "c": 1},
        correspondingRegularKeyPattern: {a: -1, b: 1, c: 1},
    },
    {
        keyPattern: {a: 1, "$**": 1, b: -1},
        wildcardProjection: {"d": 1},
        query: {a: 1, b: 1, "d": 1},
        correspondingRegularKeyPattern: {a: 1, d: 1, b: -1},
    },
    {
        keyPattern: {"$**": 1, a: 1, b: 1},
        wildcardProjection: {"front": 1},
        query: {a: 1, b: 1, "front": 1},
        correspondingRegularKeyPattern: {a: 1, b: 1, front: 1},
    },
];

// create indexes
for (const cwiFilter of cwiFilterList) {
    WildcardIndexHelpers.createIndex(coll, cwiFilter);
}

let expectedNumberOfFilters = 0;

// create and validate filters using indexes' key patterns
for (const cwiFilter of cwiFilterList) {
    assert.commandWorked(db.runCommand({
        planCacheSetFilter: collectionName,
        query: cwiFilter.query,
        indexes: [cwiFilter.keyPattern],
    }));

    expectedNumberOfFilters += 1;

    const filters = assert.commandWorked(db.runCommand({planCacheListFilters: collectionName}));
    assert.eq(expectedNumberOfFilters, filters.filters.length, filters);

    assert.neq(null, findFilter(cwiFilter, filters.filters), filters.filters);
}

// clear and validate filters using indexes' key patterns
for (const cwiFilter of cwiFilterList) {
    assert.commandWorked(db.runCommand({
        planCacheClearFilters: collectionName,
        query: cwiFilter.query,
        indexes: [cwiFilter.keyPattern],
    }));

    expectedNumberOfFilters -= 1;

    const filters = assert.commandWorked(db.runCommand({planCacheListFilters: collectionName}));
    assert.eq(expectedNumberOfFilters, filters.filters.length, filters);

    assert.eq(null, findFilter(cwiFilter, filters.filters), filters.filters);
}

// create and validate filters using indexes' names
for (const cwiFilter of cwiFilterList) {
    assert.commandWorked(db.runCommand({
        planCacheSetFilter: collectionName,
        query: cwiFilter.query,
        indexes: [cwiFilter.indexName],
    }));

    expectedNumberOfFilters += 1;

    const filters = assert.commandWorked(db.runCommand({planCacheListFilters: collectionName}));
    assert.eq(expectedNumberOfFilters, filters.filters.length, filters);

    assert.neq(null, findFilter(cwiFilter, filters.filters), filters.filters);
}

// clear and validate filters using indexes' names
for (const cwiFilter of cwiFilterList) {
    assert.commandWorked(db.runCommand({
        planCacheClearFilters: collectionName,
        query: cwiFilter.query,
        indexes: [cwiFilter.indexName],
    }));

    expectedNumberOfFilters -= 1;

    const filters = assert.commandWorked(db.runCommand({planCacheListFilters: collectionName}));
    assert.eq(expectedNumberOfFilters, filters.filters.length, filters);

    assert.eq(null, findFilter(cwiFilter, filters.filters), filters.filters);
}

// Create regular indexes.
for (const cwiFilter of cwiFilterList) {
    assert.commandWorked(coll.createIndex(cwiFilter.correspondingRegularKeyPattern));
}

// Test that CWI obey Index Filters.
for (const cwiFilter of cwiFilterList) {
    assertExpectedIndexAnswersQueryWithFilter(
        coll, cwiFilter.query, [cwiFilter.keyPattern], cwiFilter.query, cwiFilter.indexName);
}
