/**
 * This is the test for $documents stage in aggregation pipeline on a sharded collection.
 * @tags: [ do_not_wrap_aggregations_in_facets, requires_fcv_51 ]
 *
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For resultsEq.

let st = new ShardingTest({shards: 2});
const db = st.s.getDB(jsTestName());
const dbName = db.getName();
assert.commandWorked(db.adminCommand({enableSharding: dbName}));

// Create sharded collections.
const coll = db['shardedColl'];
st.shardColl(coll, {x: 1}, {x: 1}, {x: 1}, dbName);

const lookup_coll = db['lookupColl'];
st.shardColl(lookup_coll, {id_name: 1}, {id_name: 1}, {id_name: 1}, dbName);
for (let i = 0; i < 10; i++) {
    assert.commandWorked(lookup_coll.insert({id_name: i, name: "name_" + i}));
}

// $documents given an array of objects.
const docs = db.aggregate([{$documents: [{a1: 1}, {a1: 2}]}]).toArray();

assert.eq(2, docs.length);
assert.eq(docs[0], {a1: 1});
assert.eq(docs[1], {a1: 2});

// $documents evaluates to an array of objects.
const docs1 =
    db.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}]).toArray();

assert.eq(100, docs1.length);
for (let i = 0; i < 100; i++) {
    assert.eq(docs1[i], {x: i});
}

// $documents evaluates to an array of objects.
const docsUnionWith =
    coll.aggregate([
            {
                $unionWith: {
                    pipeline: [{$documents: {$map: {input: {$range: [0, 5]}, in : {x: "$$this"}}}}]
                }
            },
            {$group: {_id: "$x", x: {$first: "$x"}}},
            {$project: {_id: 0}},
        ])
        .toArray();
assert(resultsEq([{x: 0}, {x: 1}, {x: 2}, {x: 3}, {x: 4}], docsUnionWith));

{  // $documents with const objects inside $unionWith.
    const res = coll.aggregate([
                        {$unionWith: {pipeline: [{$documents: [{x: 1}, {x: 2}]}]}},
                        {$group: {_id: "$x", x: {$first: "$x"}}},
                        {$project: {_id: 0}}
                    ])
                    .toArray();
    assert(resultsEq([{x: 1}, {x: 2}], res));
}

{  // $documents with const objects inside $lookup (no "coll", explicit $match).
    const res = lookup_coll.aggregate([
                {
                    $lookup: {
                        let: {"id_lookup": "$id_name"},
                        pipeline: [
                            {$documents: [{xx: 1}, {xx: 2}, {xx : 3}]},
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
            ]
           )
            .toArray();
    assert(resultsEq(
        [
            {id_name: 1, name: "name_1", names: [{"xx": 1}]},
            {id_name: 2, name: "name_2", names: [{"xx": 2}]},
            {id_name: 3, name: "name_3", names: [{"xx": 3}]}
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
                            {$documents: [{xx: 1}, {xx: 2}, {xx: 3}]}
                        ],
                        as: "names"
                    }
                },
                {$match: {"names": {"$ne": []}}},
                {$project: {_id: 0}}
            ])
            .toArray();
    assert(resultsEq(
        [
            {id_name: 1, name: "name_1", names: [{"xx": 1}]},
            {id_name: 2, name: "name_2", names: [{"xx": 2}]},
            {id_name: 3, name: "name_3", names: [{"xx": 3}]}
        ],
        res));
}

// Must fail when $document appears in the top level collection pipeline.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}]);
}, ErrorCodes.InvalidNamespace);

// Must fail due to misplaced $document.
assert.throwsWithCode(() => {
    coll.aggregate([{$project: {x: [{xx: 1}, {xx: 2}]}}, {$documents: [{x: 1}]}]);
}, 40602);

// Test that $documents fails due to producing array of non-objects.
assert.throwsWithCode(() => {
    db.aggregate([{$documents: [1, 2, 3]}]);
}, 40228);

// Now with one object and one scalar.
assert.throwsWithCode(() => {
    db.aggregate([{$documents: [{x: 1}, 2]}]);
}, 40228);

// Test that $documents fails due when provided a non-array.
assert.throwsWithCode(() => {
    db.aggregate([{$documents: "string"}]);
}, 5858203);

// Test that $documents succeeds when given a singleton object.
assert.eq(db.aggregate([{$documents: [{x: [1, 2, 3]}]}]).toArray(), [{x: [1, 2, 3]}]);

// Must fail when $document appears in the top level collection pipeline.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}]);
}, ErrorCodes.InvalidNamespace);

st.stop();
})();
