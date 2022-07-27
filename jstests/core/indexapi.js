// Cannot implicitly shard accessed collections because of not being able to create unique index
// using hashed shard key pattern.
// @tags: [
//     cannot_create_unique_index_when_using_hashed_shard_key,
//     # Uses $indexStats which is not supported in a transaction.
//     does_not_support_transactions,
// ]
(function() {
"use strict";

const coll = db.indexapi;
coll.drop();

const kTestKeyPattern = {
    x: 1
};

const indexSpecObj = {
    ns: coll._fullName,
    key: kTestKeyPattern,
    name: coll._genIndexName(kTestKeyPattern)
};
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern));

indexSpecObj.name = "bob";
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, "bob"));

indexSpecObj.name = coll._genIndexName(kTestKeyPattern);
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern));

indexSpecObj.unique = true;
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, true));
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, [true]));
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, {unique: true}));

indexSpecObj.dropDups = true;
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, [true, true]));
assert.eq(indexSpecObj, coll._indexSpec(kTestKeyPattern, {unique: true, dropDups: true}));

function getSingleIndexWithKeyPattern(keyPattern) {
    let allIndexes = coll.aggregate([
                             {$indexStats: {}},
                             {$match: {key: keyPattern}},
                             // Unnest the "$spec" field into top-level.
                             {$replaceWith: {$mergeObjects: ["$$ROOT", "$spec"]}}
                         ])
                         .toArray();
    assert.eq(allIndexes.length, 1);
    return allIndexes[0];
}

coll.createIndex(kTestKeyPattern, {unique: true});
let allIndexes = coll.getIndexes();
assert.eq(2, allIndexes.length);
assert.sameMembers([kTestKeyPattern, {_id: 1}], allIndexes.map(entry => entry.key));
let xIndex = getSingleIndexWithKeyPattern(kTestKeyPattern);
assert.eq(xIndex.key, kTestKeyPattern);
assert(xIndex.unique, xIndex);

coll.drop();
coll.createIndex(kTestKeyPattern, {unique: 1});
allIndexes = coll.getIndexes();
assert.eq(2, allIndexes.length);
xIndex = getSingleIndexWithKeyPattern(kTestKeyPattern);
assert.eq(kTestKeyPattern, xIndex.key);
assert(xIndex.unique);
}());
