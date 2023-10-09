// This is the test for $documents stage in aggregation pipeline.
// The $documents follows these rules:
// * $documents must be in the beginning of the pipeline,
// * $documents content must evaluate into an array of objects.
// @tags: [
//   do_not_wrap_aggregations_in_facets
// ]
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const dbName = jsTestName();

const currDB = db.getSiblingDB(dbName);
const coll = currDB.documents;
coll.drop();
assert.commandWorked(coll.insert({a: 1}));

const lookup_coll = currDB.lookup_coll;
lookup_coll.drop();
for (let i = 0; i < 10; i++) {
    assert.commandWorked(lookup_coll.insert({id_name: i, name: "name_" + i}));
}
// $documents given an array of objects.
const docs = currDB.aggregate([{$documents: [{a1: 1}, {a1: 2}]}]).toArray();

assert.eq(2, docs.length);
assert.eq(docs[0], {a1: 1});
assert.eq(docs[1], {a1: 2});

// $documents evaluates to an array of objects.
const docs1 =
    currDB.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}])
        .toArray();

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

assert(resultsEq([{x: null}, {x: 0}, {x: 1}, {x: 2}, {x: 3}, {x: 4}], docsUnionWith));

{  // $documents with const objects inside $unionWith (no "coll").
    const res = coll.aggregate([
                        {$unionWith: {pipeline: [{$documents: [{xx: 1}, {xx: 2}]}]}},
                        {$group: {_id: "$xx", xx: {$first: "$xx"}}},
                        {$project: {_id: 0}}
                    ])
                    .toArray();
    assert(resultsEq([{xx: null}, {xx: 1}, {xx: 2}], res));
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
            ]
           )
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
            ])
            .toArray();
    assert(resultsEq(
        [
            {id_name: 1, name: "name_1", names: [{"xx": 1}]},
            {id_name: 2, name: "name_2", names: [{"xx": 2}]}
        ],
        res));
}

// Must fail when $document appears in the top level collection pipeline.
assert.throwsWithCode(() => {
    coll.aggregate([{$documents: {$map: {input: {$range: [0, 100]}, in : {x: "$$this"}}}}]);
}, ErrorCodes.InvalidNamespace);

// Must fail due to misplaced $document.
assert.throwsWithCode(() => {
    coll.aggregate([{$project: {a: [{xx: 1}, {xx: 2}]}}, {$documents: [{a: 1}]}]);
}, 40602);

// Must fail due to misplaced $document when database doesn't exist.
const nonExistingDB = db.getSiblingDB("this_database_does_not_exist");
assert.throwsWithCode(() => {
    nonExistingDB.foobar.aggregate([{$project: {a: [{xx: 1}, {xx: 2}]}}, {$documents: [{a: 1}]}]);
}, 40602);

// Must fail due to misplaced $document when database doesn't exist and no collection specified.
assert.throwsWithCode(() => {
    nonExistingDB.aggregate([{$project: {a: [{xx: 1}, {xx: 2}]}}, {$documents: [{a: 1}]}]);
}, ErrorCodes.InvalidNamespace);

// $unionWith must fail because it requires a collection even when database does not exist
assert.throwsWithCode(
    () => {nonExistingDB.aggregate([{
        $unionWith:
            {pipeline: [{$documents: {$map: {input: {$range: [0, 5]}, in : {x: "$$this"}}}}]}
    }])},
    ErrorCodes.InvalidNamespace);

// $unionWith must fail due to no $document
assert.throwsWithCode(() => {
    coll.aggregate([{$unionWith: {pipeline: [{$project: {a: [{xx: 1}, {xx: 2}]}}]}}]);
}, ErrorCodes.FailedToParse);

// Test that $lookup fails due to no 'from' argument and no $documents stage.
assert.throwsWithCode(() => {
    coll.aggregate([
        {
            $lookup: {
                let: {"id_lookup": "$id_name"},
                as: "aa",
                pipeline: [{$project: {a: [{xx: 1}, {xx: 2}]}}]
            }
        }
        ]);
}, ErrorCodes.FailedToParse);
// Test that $lookup fails due to no 'from' argument and no pipeline field.
assert.throwsWithCode(() => {
    coll.aggregate([{$lookup: {let : {"id_lookup": "$id_name"}, as: "aa"}}]);
}, ErrorCodes.FailedToParse);

// Test that $documents fails due to producing array of non-objects.
assert.throwsWithCode(() => {
    currDB.aggregate([{$documents: [1, 2, 3]}]);
}, 40228);
// Now with one object and one scalar.
assert.throwsWithCode(() => {
    currDB.aggregate([{$documents: [{a: 1}, 2]}]);
}, 40228);

// Test that $documents fails due when provided a non-array.
assert.throwsWithCode(() => {
    currDB.aggregate([{$documents: "string"}]);
}, [40228, 5858203]);

// Test that $documents succeeds when given a singleton object.
assert.eq(currDB.aggregate([{$documents: [{a: [1, 2, 3]}]}]).toArray(), [{a: [1, 2, 3]}]);
