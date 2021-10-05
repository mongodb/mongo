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

const dbName = jsTestName();
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

// $documents given an array of objects.
const docs = currDB.aggregate([{$documents: [{a1: 1}, {a1: 2}]}], writeConcernOptions).toArray();

assert.eq(2, docs.length);
assert.eq(docs[0], {a1: 1});
assert.eq(docs[1], {a1: 2});

// $documents evaluates to an array of objects.
const docs1 =
    currDB
        .aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}],
                   writeConcernOptions)
        .toArray();

assert.eq(100, docs1.length);
for (let i = 0; i < 100; i++) {
    assert.eq(docs1[i], {x: i});
}

// $documents evaluates to an array of objects.
const docsUnionWith =
    currDB
        .aggregate(
            [
                {$documents: [{a: 13}]},
                {
                    $unionWith: {
                        pipeline:
                            [{$documents: {$map: {input: {$range: [0, 10]}, in : {x: "$$this"}}}}]
                    }
                }
            ],
            writeConcernOptions)
        .toArray();

assert.eq(11, docsUnionWith.length);
assert.eq(docsUnionWith[0], {a: 13});
for (let i = 1; i < 11; i++) {
    assert.eq(docsUnionWith[i], {x: i - 1});
}

// $documents with const objects inside $unionWith (no "coll").
const res = coll.aggregate([{$unionWith: {pipeline: [{$documents: [{xx: 1}, {xx: 2}]}]}}],
                           writeConcernOptions)
                .toArray();
assert.eq(3, res.length);
assert.eq(res[0]["a"], 1);
assert.eq(res[1], {xx: 1});
assert.eq(res[2], {xx: 2});

function assertFails(coll, pipeline, code) {
    assert.commandFailedWithCode(currDB.runCommand({
        aggregate: coll,
        pipeline: pipeline,
        writeConcern: writeConcernOptions.writeConcern,
        cursor: {}
    }),
                                 code);
}

// Must fail due to misplaced $document.
assertFails(coll.getName(), [{$project: {a: [{xx: 1}, {xx: 2}]}}, {$documents: [{a: 1}]}], 40602);
// $unionWith must fail due to no $document
assertFails(coll.getName(), [{$unionWith: {pipeline: [{$project: {a: [{xx: 1}, {xx: 2}]}}]}}], 9);

// Must fail due to $documents producing array of non-objects.
assertFails(1, [{$documents: [1, 2, 3]}], 40228);

// Must fail due $documents producing non-array.
assertFails(1, [{$documents: {a: 1}}], 5858203);

// Must fail due $documents producing array of non-objects.
assertFails(1, [{$documents: {a: [1, 2, 3]}}], 5858203);
})();
