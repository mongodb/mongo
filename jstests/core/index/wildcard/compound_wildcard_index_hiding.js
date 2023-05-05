/**
 * Tests that Compound Wildcard Indexes can be hidden and that the planner obey their
 * visibility status.
 *
 * @tags: [
 *   not_allowed_with_security_token,
 *   does_not_support_stepdowns,
 *   does_not_support_transaction,
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/wildcard_index_helpers.js");

const collectionName = "compound_wildcard_index_hiding";
const cwiList = [
    {
        keyPattern: {a: 1, b: 1, "c.$**": 1},
        query: {a: 1, b: 1, "c.a": 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {a: 1, "c.$**": 1, b: 1},
        query: {a: 1, b: 1, "c.a": 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {"c.$**": 1, a: 1, b: 1},
        query: {a: 1, b: 1, "c.a": 1},
        wildcardProjection: undefined,
    },
    {
        keyPattern: {a: -1, b: 1, "$**": 1},
        query: {a: 1, b: 1, "c.a": 1},
        wildcardProjection: {"c": 1},
    },
    {
        keyPattern: {a: 1, "$**": -1, b: 1},
        query: {a: 1, b: 1, "d": 1},
        wildcardProjection: {"d": 1},
    },
    {
        keyPattern: {"$**": 1, a: 1, b: 1},
        query: {a: 1, b: 1, "front": 1},
        wildcardProjection: {"front": 1},
    },
];

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
        WildcardIndexHelpers.createIndex(coll, indexSpec);

        expectedNumberOfIndexes += 1;
        assert.eq(expectedNumberOfIndexes, coll.getIndexes().length);
        WildcardIndexHelpers.validateIndexVisibility(coll, indexSpec);
    }

    // Toggle index visibility twice by key pattern
    for (let i = 0; i < 2; ++i) {
        for (const indexSpec of cwiList) {
            indexSpec.hidden = !indexSpec.hidden;
            setIndexVisibilityByKeyPattern(collectionName, indexSpec.keyPattern, indexSpec.hidden);
            WildcardIndexHelpers.validateIndexVisibility(coll, indexSpec);
        }
    }

    // Toggle index visibility twice by index name
    for (let i = 0; i < 2; ++i) {
        for (const indexSpec of cwiList) {
            indexSpec.hidden = !indexSpec.hidden;
            setIndexVisibilityByIndexName(collectionName, indexSpec.indexName, indexSpec.hidden);
            WildcardIndexHelpers.validateIndexVisibility(coll, indexSpec);
        }
    }

    // remove indexes
    for (const indexSpec of cwiList) {
        assert.commandWorked(coll.dropIndex(indexSpec.keyPattern));

        expectedNumberOfIndexes -= 1;
        assert.eq(expectedNumberOfIndexes, coll.getIndexes().length);
        const index = WildcardIndexHelpers.findIndex(coll, indexSpec);
        assert.eq(null, index);
    }
}

function assertHiddenIndexesIsNotUsed(cwiList, collectionName) {
    db[collectionName].drop();
    assert.commandWorked(db.createCollection(collectionName));
    const coll = db[collectionName];

    for (const indexSpec of cwiList) {
        indexSpec.hidden = true;
        WildcardIndexHelpers.createIndex(coll, indexSpec);
    }

    // All of the indexes created at the previous step are hidden and cannot answer the query. This
    // step first makes sure that a hidden index cannot answer a query, then unhide the index and
    // makes sure that not it answers the query and finally hide the index again and makes sure it
    // does not answer the query.
    // At every iteration we have maximum one index unhidden to make sure that other indexes cannot
    // answer a test query. We can use hinting to force the index to be used but that would collide
    // this test with the other test which checking hinting.
    for (const testCase of cwiList) {
        const query = testCase.query;
        const indexName = testCase.indexName;
        let explain = null;

        explain = assert.commandWorked(coll.find(query).explain('executionStats'));
        // The index is hidden and cannot be used.
        WildcardIndexHelpers.assertExpectedIndexIsNotUsed(explain, indexName);

        // Unhide the index and make sure it answers the query.
        setIndexVisibilityByIndexName(collectionName, indexName, /*hidden*/ false);
        explain = assert.commandWorked(coll.find(query).explain('executionStats'));
        WildcardIndexHelpers.assertExpectedIndexIsUsed(explain, indexName);

        // Hide the index and make sure it does not answer the query.
        setIndexVisibilityByIndexName(collectionName, indexName, /*hidden*/ true);
        explain = assert.commandWorked(coll.find(query).explain('executionStats'));
        WildcardIndexHelpers.assertExpectedIndexIsNotUsed(explain, indexName);
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

/////////////////////////////////////////////////////////////////////////
// 3. Test that queries do not use hidden Compound Wildcard Indexes.
assertHiddenIndexesIsNotUsed(cwiList, collectionName);
})();
