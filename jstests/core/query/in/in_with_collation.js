/**
 * Test $in expressions with collation.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_getmore
 * ]
 */
const coll = db.jstests_in_with_collation;
coll.drop();

// Create a collection with a collation that is case-insensitive
const caseInsensitive = {
    collation: {locale: "en_US", strength: 2}
};
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));

let inlist = [
    'cyD',
    8,
    'BZd',
    6,
    'awD',
    2,
    'Cwa',
    9,
    3,
    'bxA',
    'azB',
    4,
    'Ayc',
    1,
    'cXB',
    'dYa',
    7,
    'bwC',
    5
];

function transformValue(value, i) {
    if (typeof value === 'number' || i % 2 != 0) {
        return value;
    } else {
        return (i % 4 == 0) ? value.toLowerCase() : value.toUpperCase();
    }
}

let docs = [];
for (let i = inlist.length - 1; i >= 0; --i) {
    let id1 = i * 2;
    let x1 = transformValue(inlist[i], i);
    docs.push({_id: id1, x: x1});

    let k = (i % 2 == 0) ? 10 : -10;
    let id2 = id1 + 1;
    let x2 = (typeof x1 === 'number') ? x1 + k : x1.substr(1) + x1.substr(0, 1);
    docs.push({_id: id2, x: x2});
}

// Insert 'docs' into the collection.
assert.commandWorked(coll.insert(docs));

// Get the list of document _ids where 'x' matches one of the values in 'inlist'.
let matchingIds = coll.find({x: {$in: inlist}}, {_id: 1}).toArray().map(doc => doc._id);
matchingIds.sort((a, b) => a - b);

// Check that the list of _ids is equal to what we expect.
assert.eq(matchingIds, [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36]);
