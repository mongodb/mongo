/*
 * Test that $group works when an empty object is passed for _id. This is intended to reproduce
 * SERVER-89611.
 */
const coll = db.group_by_empty_obj;
coll.drop();

assert.commandWorked(coll.insert([{_id: 1, x: 1}, {_id: 2, x: 2}]));

function assertIsEmptyObjId(groupSpec) {
    assert.eq([{_id: {}}], coll.aggregate([groupSpec]).toArray());
}
assertIsEmptyObjId({$group: {_id: {}}});
assertIsEmptyObjId({$group: {_id: {$expr: {}}}});
assertIsEmptyObjId({$group: {_id: {$expr: {$const: {}}}}});
assertIsEmptyObjId({$group: {_id: {$expr: {$expr: {}}}}});

// The original fuzzer failure involved a $sortByCount query
assert.eq([{_id: {}, count: 2}], coll.aggregate([{$sortByCount: {$expr: {}}}]).toArray());
