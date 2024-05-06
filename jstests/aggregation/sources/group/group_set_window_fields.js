/*
 * Test that $group works when Nothing is passed in for _id. This is intended to reproduce
 * SERVER-90185.
 */
const coll = db.group_set_window_fields;
coll.drop();

assert.commandWorked(coll.insert({_id: 1}));

const pipeline =
    [{$setWindowFields: {output: {obj: {$first: null}}}}, {$group: {_id: "$obj.nonexistent"}}];

const expected = [{_id: null}];

const results = coll.aggregate(pipeline).toArray();
assert.eq(results, expected);
