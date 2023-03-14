/**
 * Tests that Compound Wildcard Indexes indexes obey hinting.
 * @tags: [
 *   assumes_read_concern_local,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */

(function() {

load("jstests/libs/wildcard_index_helpers.js");

const cwiList = [
    // Note: 'wildcardProjection' cannot be specified if the wildcard field is not "$**".
    {
        keyPattern: {a: 1, b: -1, "c.$**": 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.d": 1},
        corresponingRegularKeyPattern: {a: 1, b: -1, "c.d": 1},
    },
    {
        keyPattern: {a: -1, "c.$**": 1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.d": 1},
        corresponingRegularKeyPattern: {a: -1, "c.d": 1, b: 1},
    },
    {
        keyPattern: {"c.$**": 1, a: 1, b: 1},
        wildcardProjection: undefined,
        query: {a: 1, b: 1, "c.d": 1},
        corresponingRegularKeyPattern: {"c.d": 1, a: 1, b: 1},
    },
    {
        keyPattern: {a: 1, b: 1, "$**": -1},
        wildcardProjection: {"c": 1},
        query: {a: 1, b: 1, "c": 1},
        corresponingRegularKeyPattern: {a: 1, b: 1, c: -1},
    },
    {
        keyPattern: {a: 1, "$**": 1, b: 1},
        wildcardProjection: {"c": 1},
        query: {a: 1, b: 1, "c": 1},
        corresponingRegularKeyPattern: {a: 1, d: 1, b: 1},
    },
    {
        keyPattern: {"$**": 1, a: 1, b: 1},
        wildcardProjection: {"c": 1},
        query: {a: 1, b: 1, "c": 1},
        corresponingRegularKeyPattern: {a: 1, b: 1, front: 1},
    },
];

const collectionName = 'compound_wildcard_index_hint';
db[collectionName].drop();
assert.commandWorked(db.createCollection(collectionName));
const coll = db[collectionName];

//  Create indexes.
for (const indexSpec of cwiList) {
    WildcardIndexHelpers.createIndex(coll, indexSpec);
    assert.commandWorked(coll.createIndex(indexSpec.corresponingRegularKeyPattern));
}

// Test that CWIs obey hinting using index name.
for (const testCase of cwiList) {
    const explain = assert.commandWorked(
        coll.find(testCase.query).hint(testCase.indexName).explain('executionStats'));
    WildcardIndexHelpers.assertExpectedIndexIsUsed(explain, testCase.indexName);
}

// Test that CWIs obey hinting using index key pattern.
for (const testCase of cwiList) {
    const explain = assert.commandWorked(
        coll.find(testCase.query).hint(testCase.keyPattern).explain('executionStats'));
    WildcardIndexHelpers.assertExpectedIndexIsUsed(explain, testCase.indexName);
}
})();
