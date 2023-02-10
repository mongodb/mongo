/**
 * Tests that Compound Wildcard Indexes can be hidden.
 *
 * @tags: [
 *   not_allowed_with_security_token,
 *   does_not_support_stepdowns,
 *   featureFlagCompoundWildcardIndexes,
 * ]
 */

(function() {
"use strict";

const collectionName = "compound_wildcard_index_hiding";
const cwiList = [
    {
        keyPattern: {a: 1, b: 1, "c.$**": 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {a: 1, "c.$**": 1, b: 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {"c.$**": 1, a: 1, b: 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {a: 1, b: 1, "$**": 1},
        wildcardProjection: {"c": 1},
    },
    {
        keyPattern: {a: 1, "$**": 1, b: 1},
        wildcardProjection: {"d": 1},
    },
    {
        keyPattern: {"$**": 1, a: 1, b: 1},
        wildcardProjection: {"front": 1},
    },
];

function getIndexName(coll, keyPattern) {
    const indexes = coll.getIndexes();
    const index = indexes.find(index => bsonWoCompare(index.key, keyPattern) == 0);
    if (index !== undefined) {
        return index.name;
    }
    return null;
}

function findIndex(coll, indexSpec) {
    const indexes = coll.getIndexes();

    for (const index of indexes) {
        if (bsonWoCompare(index.key, indexSpec.keyPattern) == 0) {
            return index;
        }
    }

    return null;
}

function createIndex(coll, indexSpec) {
    const options = {};
    if (indexSpec.wildcardProjection) {
        options["wildcardProjection"] = indexSpec.wildcardProjection;
    }
    if (indexSpec.hidden) {
        options["hidden"] = true;
    }

    assert.commandWorked(coll.createIndex(indexSpec.keyPattern, options));
    indexSpec.indexName = getIndexName(coll, indexSpec.keyPattern);
    assert.neq(null, indexSpec.indexName);
}

function validateIndex(coll, indexSpec) {
    const index = findIndex(coll, indexSpec);
    assert.neq(null, index);

    if (indexSpec.hidden) {
        assert.eq(true, index.hidden);
    } else {
        assert.neq(true, index.hidden);
    }
}

function setIndexVisibilityByKeyPattern(collectionName, keyPattern, hidden) {
    assert.commandWorked(db.runCommand({collMod: collectionName, index: {keyPattern, hidden}}));
}

function setIndexVisibilityByIndexName(collectionName, indexName, hidden) {
    assert.commandWorked(
        db.runCommand({collMod: collectionName, index: {name: indexName, hidden}}));
}

function testCompoundWildcardIndexesHiding(cwiList, collectionName) {
    db[collectionName].drop();
    assert.commandWorked(db.createCollection(collectionName));
    const coll = db[collectionName];
    let expectedNumberOfIndexes = coll.getIndexes().length;

    // create indexes
    for (const indexSpec of cwiList) {
        createIndex(coll, indexSpec);

        expectedNumberOfIndexes += 1;
        assert.eq(expectedNumberOfIndexes, coll.getIndexes().length);
        validateIndex(coll, indexSpec);
    }

    // Toggle index visibility twice by key pattern
    for (let i = 0; i < 2; ++i) {
        for (const indexSpec of cwiList) {
            indexSpec.hidden = !indexSpec.hidden;
            setIndexVisibilityByKeyPattern(collectionName, indexSpec.keyPattern, indexSpec.hidden);
            validateIndex(coll, indexSpec);
        }
    }

    // Toggle index visibility twice by index name
    for (let i = 0; i < 2; ++i) {
        for (const indexSpec of cwiList) {
            indexSpec.hidden = !indexSpec.hidden;
            setIndexVisibilityByIndexName(collectionName, indexSpec.indexName, indexSpec.hidden);
            validateIndex(coll, indexSpec);
        }
    }

    // remove indexes
    for (const indexSpec of cwiList) {
        assert.commandWorked(coll.dropIndex(indexSpec.keyPattern));

        expectedNumberOfIndexes -= 1;
        assert.eq(expectedNumberOfIndexes, coll.getIndexes().length);
        const index = findIndex(coll, indexSpec);
        assert.eq(null, index);
    }
}

/////////////////////////////////////////////////////////////////////////
// 1. Create, hide, unhide, and delete Compound Wildcard Indexes.

for (const index of cwiList) {
    index["hidden"] = false;
}
testCompoundWildcardIndexesHiding(cwiList, collectionName);

/////////////////////////////////////////////////////////////////////////
// 2. Create hidden, unhide, hide, and delete Compound Wildcard Indexes.

for (const index of cwiList) {
    index["hidden"] = true;
}
testCompoundWildcardIndexesHiding(cwiList, collectionName);
})();
