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
(function() {
"use strict";

const coll = db.double_optimize;
coll.drop();

assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));
// Test that this $match can be split in two and partially swapped before the $project stage. This
// will test that the $match stage gets optimized and removes the $or to realize that the predicate
// is independent of the field "b".
const inputPipe = [{$project: {b: 0}}, {$match: {$or: [{_id: 4, b: 3}]}}];
const explain = coll.explain().aggregate(inputPipe);
const lastStage = explain.stages[explain.stages.length - 1];
assert(lastStage.hasOwnProperty("$match"), tojson(explain));
assert.eq({$match: {b: {$eq: 3}}},
          lastStage,
          "The $match stage should have been split in two and moved in front of the $project. " +
              explain);
}());
