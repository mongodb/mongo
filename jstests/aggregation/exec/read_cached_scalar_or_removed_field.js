/**
 * SERVER-90871, SERVER-91118: When a document field is updated to a scalar or is removed and later
 * we sort on a sub field of that, we should return missing instead of the value in the backing
 * BSON.
 */

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 1, "obj": {"obj": {}}},
    {_id: 2},
]));

// Update a field to scalar
let results =
    coll.aggregate([{$addFields: {"obj": null}}, {$sort: {"obj.obj": 1, _id: 1}}]).toArray();
assert.eq(results, [{_id: 1, "obj": null}, {_id: 2, "obj": null}], results);

// Remove a field
results = coll.aggregate([{$project: {"obj": 0}}, {$sort: {"obj.obj": 1, _id: 1}}]).toArray();
assert.eq(results, [{_id: 1}, {_id: 2}], results);
