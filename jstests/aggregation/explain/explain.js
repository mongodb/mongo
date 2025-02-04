// Tests the behavior of explain() when used with the aggregation pipeline.
// - Explain() should not read or modify the plan cache.
// - The result should always include serverInfo.
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";

let coll = db.explain;
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

let result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));
assert(result.hasOwnProperty('serverInfo'), result);
assert.hasFields(result.serverInfo, ['host', 'port', 'version', 'gitVersion']);

// At this point, there should be no entries in the plan cache.
result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));

// Now add entry in the cache without explain().
result = coll.aggregate([{$match: {x: 1, y: 1}}]);

// Now there's an entry in the cache, make sure explain() doesn't use it.
result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));

// At the time of this writing there are times when an entire aggregation pipeline can be absorbed
// into the query layer. In these cases we use a different explain mechanism. Using $lookup will
// prevent this optimization and stress an explain implementation in the aggregation layer. Test
// that this implementation also includes serverInfo.
result = coll.explain().aggregate([{$lookup: {from: 'other_coll', pipeline: [], as: 'docs'}}]);
assert(result.hasOwnProperty('serverInfo'), result);
assert.hasFields(result.serverInfo, ['host', 'port', 'version', 'gitVersion']);