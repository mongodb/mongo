/**
 * SERVER-90871: When a document update a field to scalar and later we sort on a sub field of that,
 * we should return missing instead of the value in the backing BSON.
 */

const coll = db.server90871;
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 1, "obj": {"obj": {}}},
    {_id: 2},
]));
const results =
    coll.aggregate([{$addFields: {"obj": null}}, {$sort: {"obj.obj": 1, _id: 1}}]).toArray();
assert.eq(results, [{_id: 1, "obj": null}, {_id: 2, "obj": null}], results);
