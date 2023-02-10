/**
 * Tests that Compound Wildcard Indexes can be added/removed/listed from index filters.
 *
 * @tags: [
 *   not_allowed_with_security_token,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 * ]
 */

(function() {
"use strict";

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

function getIndexName(coll, keyPattern) {
    const indexes = coll.getIndexes();
    const index = indexes.find(index => bsonWoCompare(index.key, keyPattern) == 0);
    if (index !== undefined) {
        return index.name;
    }
    return null;
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
    },
    {
        keyPattern: {a: 1, "c.$**": 1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.a": 1},
    },
    {
        keyPattern: {"c.$**": 1, a: 1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.front": 1},
    },
    {
        keyPattern: {a: 1, b: 1, "$**": 1},
        wildcardProjection: {"c": 1},
        query: {a: 1, b: 1, "c": 1},
    },
    {
        keyPattern: {a: 1, "$**": 1, b: 1},
        wildcardProjection: {"d": 1},
        query: {a: 1, b: 1, "d": 1},
    },
    {
        keyPattern: {"$**": 1, a: 1, b: 1},
        wildcardProjection: {"front": 1},
        query: {a: 1, b: 1, "front": 1},
    },
];

// create indexes
for (const cwiFilter of cwiFilterList) {
    const options = {};
    if (cwiFilter.wildcardProjection) {
        options["wildcardProjection"] = cwiFilter.wildcardProjection;
    }

    assert.commandWorked(coll.createIndex(cwiFilter.keyPattern, options));
    cwiFilter.indexName = getIndexName(coll, cwiFilter.keyPattern);
    assert.neq(null, cwiFilter.indexName);
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
})();
