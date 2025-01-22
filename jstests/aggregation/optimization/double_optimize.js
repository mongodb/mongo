// This test was written to reproduce SERVER-44258 and prove that a band-aid approach is having the
// desired effect. It should be ported to a unit test once SERVER-44258 is tackled more
// holistically.
//
// TODO SERVER-44258 delete this test and replace it with a unit test.
//
// @tags: [
//   # Tests the 'stages' field of the explain output which is hidden beneath each shard's name when
//   # run against sharded collections.
//   assumes_unsharded_collection,
//   # Tests the explain output, so does not work when wrapped in a facet.
//   do_not_wrap_aggregations_in_facets,
//   # Explicitly testing optimization.
//   requires_pipeline_optimization,
// ]
const coll = db.double_optimize;
coll.drop();

assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));
// Test that this $match can be split in two and partially swapped before the $addFields stage. We
// expect optimization to examine the $or and observe that the {_id: 4} portion of the predicate is
// independent of the "trap" field and can be evaluated before the "trap" computation.
//
// If the optimizer fails to move the {_id: 4} filter to the front of the pipeline, the $addFields
// stage will evaluate sqrt(_id - 4) on every document, resulting in a fatal error when (_id - 4) is
// negative.
const inputPipe = [
    {$addFields: {trap: {$sqrt: {$subtract: ["$_id", 4]}}}},
    {$match: {$or: [{_id: 4, trap: {$ne: 0}}]}}
];
const result = coll.aggregate(inputPipe).toArray();
assert.sameMembers([], result);
