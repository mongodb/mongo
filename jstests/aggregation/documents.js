// This is the test for $documents stage in aggregation pipeline.
// The $documents follows these rules:
// * $documents must be in the beginning of the pipeline,
// * $documents content must evaluate into an array of objects.
// $documents is not meant to be used in sharded env yet. It is going to return
//  the same result set for each shard which is counter intuitive. The test is disabled
//  for mongos
// @tags: [
//   do_not_wrap_aggregations_in_facets,
//   assumes_unsharded_collection,
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   assumes_against_mongod_not_mongos
// ]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For resultsEq.

const dbName = jsTestName();
// TODO SERVER-59097 - expose $documents and get rid of internal
// client here
const writeConcernOptions = {
    writeConcern: {w: "majority"}
};

const testInternalClient = (function createInternalClient() {
    const connInternal = new Mongo(db.getMongo().host);
    const curDB = connInternal.getDB(dbName);
    assert.commandWorked(curDB.runCommand({
        "hello": 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
    }));
    return connInternal;
})();

const currDB = testInternalClient.getDB(dbName);
const coll = currDB.documents;
coll.drop(writeConcernOptions);
coll.insert({a: 1}, writeConcernOptions);

const lookup_coll = currDB.lookup_coll;
lookup_coll.drop(writeConcernOptions);
for (let i = 0; i < 10; i++) {
    lookup_coll.insert({id_name: i, name: "name_" + i}, writeConcernOptions);
}
// $documents given an array of objects.
const docs = coll.aggregate([{$documents: [{a1: 1}, {a1: 2}]}], writeConcernOptions).toArray();

assert.eq(2, docs.length);
assert.eq(docs[0], {a1: 1});
assert.eq(docs[1], {a1: 2});

// $documents evaluates to an array of objects.
const docs1 =
    coll.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}],
                   writeConcernOptions)
        .toArray();

assert.eq(100, docs1.length);
for (let i = 0; i < 100; i++) {
    assert.eq(docs1[i], {x: i});
}

// $documents evaluates to an array of objects.
const docsUnionWith =
    coll.aggregate(
            [
                {$documents: [{a: 13}]},
                {
                    $unionWith: {
                        pipeline:
                            [{$documents: {$map: {input: {$range: [0, 5]}, in : {x: "$$this"}}}}]
                    }
                }
            ],
            writeConcernOptions)
        .toArray();

assert(resultsEq([{a: 13}, {x: 0}, {x: 1}, {x: 2}, {x: 3}, {x: 4}], docsUnionWith));

{  // $documents with const objects inside $unionWith (no "coll").
    const res = coll.aggregate(
                        [
                            {$unionWith: {pipeline: [{$documents: [{xx: 1}, {xx: 2}]}]}},
                            {$project: {_id: 0}}
                        ],
                        writeConcernOptions)
                    .toArray();
    assert(resultsEq([{a: 1}, {xx: 1}, {xx: 2}], res));
}

{  // $documents with const objects inside $lookup (no "coll", explicit $match).
    const res = lookup_coll.aggregate([
                {
                    $lookup: {
                        let: {"id_lookup": "$id_name"},
                        pipeline: [
                            {$documents: [{xx: 1}, {xx: 2}]},
                            {
                                $match:
                                    {
                                        $expr:
                                            {
                                                $eq:
                                                    ["$$id_lookup", "$xx"]
                                            }
                                    }
                            }
                            ],
                        as: "names"
                    }
                },
                {$match: {"names": {"$ne": []}}},
                {$project: {_id: 0}}
            ],
            writeConcernOptions)
            .toArray();
    assert(resultsEq(
        [
            {id_name: 1, name: "name_1", names: [{"xx": 1}]},
            {id_name: 2, name: "name_2", names: [{"xx": 2}]}
        ],
        res));
}

{  // $documents with const objects inside $lookup (no "coll", + localField/foreignField).
    const res = lookup_coll.aggregate([
                {
                    $lookup: {
                        localField: "id_name",
                        foreignField: "xx",
                        pipeline: [
                            {$documents: [{xx: 1}, {xx: 2}]}
                        ],
                        as: "names"
                    }
                },
                {$match: {"names": {"$ne": []}}},
                {$project: {_id: 0}}
            ],
            writeConcernOptions)
            .toArray();
    assert(resultsEq(
        [
            {id_name: 1, name: "name_1", names: [{"xx": 1}]},
            {id_name: 2, name: "name_2", names: [{"xx": 2}]}
        ],
        res));
}

// Must fail due to misplaced $document.
assert.throwsWithCode(() => {
    coll.aggregate([{$project: {a: [{xx: 1}, {xx: 2}]}}, {$documents: [{a: 1}]}],
                   writeConcernOptions);
}, 40602);

// $unionWith must fail due to no $document
assert.throwsWithCode(() => {
    coll.aggregate([{$unionWith: {pipeline: [{$project: {a: [{xx: 1}, {xx: 2}]}}]}}],
                   writeConcernOptions);
}, 9);

// Test that $lookup fails due to no 'from' argument and no $documents stage.
assert.throwsWithCode(() => {
    coll.aggregate([{$lookup: {let: {"id_lookup": "$id_name"}, as: "aa", pipeline: [{$project: {a: [{xx: 1}, {xx: 2}]}}]}}],
        writeConcernOptions);
}, 9);
// Test that $lookup fails due to no 'from' argument and no pipeline field.
assert.throwsWithCode(() => {
    coll.aggregate([{$lookup: {let : {"id_lookup": "$id_name"}, as: "aa"}}], writeConcernOptions);
}, 9);
// Must fail due to $documents producing array of non-objects.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: [1, 2, 3]}], writeConcernOptions);
}, 40228);

// Must fail due $documents producing non-array.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: {a: 1}}], writeConcernOptions);
}, 5858203);

// Must fail due $documents producing array of non-objects.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: {a: [1, 2, 3]}}], writeConcernOptions);
}, 5858203);
})();
