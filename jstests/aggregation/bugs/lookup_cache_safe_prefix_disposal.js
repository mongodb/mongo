// This test was designed to reproduce SERVER-59435. A $lookup sub-pipeline which is entirely
// uncorrelated can have its results be cached and re-used for subsequent $lookups. To do this we
// insert a special caching stage in the $lookup sub-pipeline at a place where everything "in front
// of it" or "to the left" is uncorrelated (the same for each lookup). For the first lookup, the
// cache just sits there and collects results. In the second lookup it is inserted in the same way
// to the new pipeline (which may be different if there were correlated pieces that need to be
// updated on each lookup), and then after being inserted it *deletes* the prefix of the pipeline
// which is no longer needed because the results will now come from the cache. There was a bug where
// this deletion happened in a state where the pipeline was not prepared for destruction and
// followed an old/invalid pointer. This test case reproduces the scenario that discovered this bug
// and is simply designed to demonstrate that we no longer crash.
//
// @tags: [
//   # $lookup containing $facet is not allowed in $facet (because $facet is not allowed in $facet).
//   do_not_wrap_aggregations_in_facets,
// ]
(function() {
"use strict";

const joinColl = db.lookup_non_correlated_prefix_join;
const testColl = db.lookup_non_correlated_prefix;
joinColl.drop();
testColl.drop();

// We need two documents to make sure we have time to (1) populate the cache and (2) attempt to
// re-use the cache.
testColl.insert([{}, {}]);

assert.doesNotThrow(() => testColl.aggregate([{
  $lookup: {
    as: 'items_check',
    from: joinColl.getName(),
    let: { id: '$_id' },
    pipeline: [
      // This pipeline is interesting - the $match stage will swap before the $addFields. In doing
      // so, it will create a copy and destroy itself, which will leave $facet with a dangling
      // pointer to an old $match stage which is no longer valid.
      { $addFields: { id: '$_id' } },
      { $match: {} },
      { $facet: { all: [{ $match: {} }] } }
    ]
  }
}]));
}());
