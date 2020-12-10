// @tags: [
//   requires_replication,
//   requires_sharding,
//   sbe_incompatible,
// ]
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');

const st = new ShardingTest({shards: 1, rs: {nodes: 1}, config: 1});
const db = st.s.getDB("test");
const coll = db.getCollection('coll');

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
st.shardColl(coll, {a: 1, b: 1}, false);

// SERVER-39241 One plan uses the {a: 1} index, which isn't enough to cover the query, because the
// sharding filter needs check the value of b. The other plan uses the {a: 1, b: 1}, which does
// cover the query. Assert the covered plan wins.
let explain = coll.explain().count({a: 1});
assert(planHasStage(db, explain, 'SHARDING_FILTER'), explain);
assert(isIndexOnly(db, explain), explain);

let rejected = getRejectedPlans(explain);
assert.eq(rejected.length, 1, rejected);
assert(planHasStage(db, rejected[0], 'SHARDING_FILTER'), explain);
assert(planHasStage(db, rejected[0], 'FETCH'), rejected);

st.stop();
}());
