// Facet in a lookup cannot be wrapped in a facet.
// @tags: [do_not_wrap_aggregations_in_facets]

/**
 * Confirms that $lookup with a non-correlated prefix returns expected results.
 */
(function() {
"use strict";

const testColl = db.lookup_non_correlated_prefix;
testColl.drop();
const joinColl = db.lookup_non_correlated_prefix_join;
joinColl.drop();

const users = [
    {
        _id: "user_1",
    },
    {
        _id: "user_2",
    },
];
let res = assert.commandWorked(testColl.insert(users));

const items = [
    {_id: "item_1", owner: "user_1"},
    {_id: "item_2", owner: "user_2"},
];
res = assert.commandWorked(joinColl.insert(items));

// $lookup with non-correlated prefix followed by correlated pipeline suffix containing $facet
// returns correct results. This test confirms the fix for SERVER-41714.
let cursor = testColl.aggregate([
    {
        $lookup: {
            as: 'items_check',
            from: joinColl.getName(),
            let : {id: '$_id'},
            pipeline: [
                {$addFields: {id: '$_id'}},
                {$match: {$expr: {$eq: ['$$id', '$owner']}}},
                {
                    $facet: {
                        all: [{$match: {}}],
                    },
                },
            ],
        },
    },
]);
assert(cursor.hasNext());
cursor.toArray().forEach(user => {
    const joinedDocs = user['items_check'][0]['all'];
    assert.neq(null, joinedDocs);
    assert.eq(1, joinedDocs.length);
    assert.eq(user['_id'], joinedDocs[0].owner);
});

cursor = testColl.aggregate([
    {
        $lookup: {
            as: 'items_check',
            from: joinColl.getName(),
            let : {id: '$_id'},
            pipeline: [
                {$addFields: {id: '$_id'}},
                {$match: {$expr: {$eq: ['$$id', '$owner']}}},
            ],
        },
    },
]);
assert(cursor.hasNext());
cursor.toArray().forEach(user => {
    const joinedDocs = user['items_check'];
    assert.neq(null, joinedDocs);
    assert.eq(1, joinedDocs.length);
    assert.eq(user['_id'], joinedDocs[0].owner);
});

// SERVER-57000: Test handling of lack of correlation (addFields with empty set of columns)
assert.doesNotThrow(() => testColl.aggregate([
    {
        $lookup: {
            as: 'items_check',
            from: joinColl.getName(),
            pipeline: [
                {$addFields: {}},
                {
                    $facet: {
                        all: [{$match: {}}],
                    },
                },
            ],
        },
    },
]));
})();
