// Test that $out can write to the same collection from which it reads.
//
// This test assumes that collections are not implicitly sharded, since $out is prohibited if the
// output collection is sharded.
// @tags: [assumes_unsharded_collection]
(function() {
"use strict";

const coll = db.out_read_write_to_same_collection;
coll.drop();
assert.commandWorked(coll.insert(
    [{group: 1, count: 1}, {group: 1, count: 3}, {group: 2, count: 4}, {group: 2, count: 8}]));

assert.eq(
    0,
    coll.aggregate([{$group: {_id: "$group", total: {$sum: "$count"}}}, {$out: coll.getName()}])
        .itcount());
assert.eq(coll.find().sort({_id: 1}).toArray(), [{_id: 1, total: 4}, {_id: 2, total: 12}]);

// Create a secondary index on the collection and add a document validator.
assert.commandWorked(coll.createIndex({total: 1}));
assert.commandWorked(db.runCommand({
    collMod: coll.getName(),
    validator: {$jsonSchema: {bsonType: "object", required: ["_id", "total"]}},
}));

// Attempt an $out which both reads from and writes to 'coll' that would violate the validator. This
// should fail, and leave the original contents of the collection in place.
assert.throws(() => coll.aggregate([{$project: {total: 0}}, {$out: coll.getName()}]).itcount());
assert.eq(coll.find().sort({_id: 1}).toArray(), [{_id: 1, total: 4}, {_id: 2, total: 12}]);

// Attempt an $out that succeeds and verify that the secondary index and collection validator are
// still present.
assert.eq(0, coll.aggregate([{$addFields: {myConstant: 42}}, {$out: coll.getName()}]).itcount());
assert.eq(coll.find().sort({_id: 1}).toArray(),
          [{_id: 1, total: 4, myConstant: 42}, {_id: 2, total: 12, myConstant: 42}]);
const collMetadata = db.getCollectionInfos().find((collectionMetadata) =>
                                                      collectionMetadata.name === coll.getName());
assert.eq(collMetadata.options.validator,
          {$jsonSchema: {bsonType: "object", required: ["_id", "total"]}},
          collMetadata);
const indexMetadata =
    coll.aggregate(
            [{$indexStats: {}}, {$project: {key: 1}}, {$sort: {key: 1}}, {$replaceWith: "$key"}])
        .toArray();
assert.eq(indexMetadata, [{_id: 1}, {total: 1}]);

coll.drop();
}());
