// @tags: [
//     # Aggregation(used by writes without shard key) does not support $where.
//     assumes_unsharded_collection,
//
//     # Uses $where operator
//     requires_scripting,
//     # TODO(SERVER-84158): Try to include this test(s).
//     exclude_from_timeseries_crud_passthrough,
// ]

const t = db[jsTestName()];
t.drop();

assert.commandWorked(t.insert({_id: 1, x: 1}));

let res = t.findAndModify({query: {$where: "return this.x == 1"}, update: {$set: {y: 1}}});

assert.eq(1, t.findOne().y);
