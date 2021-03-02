// Ensures TEXT_OR stage is elided or replaced with OR stage when possible.
// @tags: [
//   # We don't try to replace TEXT_OR with OR when the results are consumed by a merging node,
//   # because the shard doesn't know whether the merger needs the textScore metadata.
//   assumes_unsharded_collection,
// ]
(function() {
'use strict';

load("jstests/libs/analyze_plan.js");

const coll = db.optimize_text;
assert.commandWorked(coll.createIndex({"$**": "text"}));

const findExplain = coll.find({$text: {$search: 'banana leaf'}}).explain();
const findSingleTermExplain = coll.find({$text: {$search: 'banana'}}).explain();
const aggExplain = coll.explain().aggregate({$match: {$text: {$search: 'banana leaf'}}});

// The .find() plan doesn't have TEXT_OR, it has OR instead.
// Both kinds of stages deduplicate record IDs, but OR is better because OR is streaming
// while TEXT_OR is blocking. TEXT_OR is blocking because it has to accumulate the textScore
// of each record ID, so when textScore is unused we can use the more efficient OR.
assert(planHasStage(db, findExplain, 'OR'), findExplain);
assert(!planHasStage(db, findExplain, 'TEXT_OR'), findExplain);

// The same optimization should apply to the equivalent aggregation query. Before SERVER-47848 we
// failed to apply this optimization because we were too conservative about dependency tracking.
// Specifically, before SERVER-47848 we assumed that the consumer of a pipeline might depend on
// textScore if all the stages preserve it. We made that assumption because in a sharded cluster,
// when a merger sends the shardsPart of a pipeline to the shards, it doesn't specify whether it
// depends on textScore. After SERVER-47848, we refined that assumption: the shard can tell whether
// the consumer is a merger by looking at needsMerge. If the consumer is not a merger, there won't
// be an implicit dependency on textScore.
assert(planHasStage(db, aggExplain, 'OR'), aggExplain);
assert(!planHasStage(db, aggExplain, 'TEXT_OR'), aggExplain);

// Non-blocking $text plans with just one search term do not need an OR stage, as a further
// optimization.
assert(!planHasStage(db, findSingleTermExplain, 'OR'), findSingleTermExplain);
assert(!planHasStage(db, findSingleTermExplain, 'TEXT_OR'), findSingleTermExplain);
})();
