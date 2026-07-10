// @tags: [
//   # Time series collections do not support indexing array values in measurement fields.
//   exclude_from_timeseries_crud_passthrough,
//   # SERVER-36681 changed the behavior of SBE and classic engines
//   requires_fcv_90,
// ]

const coll = db.dotted_path_in_null;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, a: [{b: 5}]}));
assert.commandWorked(coll.insert({_id: 2, a: [{}]}));
assert.commandWorked(coll.insert({_id: 3, a: []}));
assert.commandWorked(coll.insert({_id: 4, a: [{}, {b: 5}]}));
assert.commandWorked(coll.insert({_id: 5, a: [5, {b: 5}]}));

function getIds(query) {
    let ids = [];
    coll.find(query)
        .sort({_id: 1})
        .forEach((doc) => ids.push(doc._id));
    return ids;
}

assert.eq([2, 3, 4, 5], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");

assert.commandWorked(coll.createIndex({"a.b": 1}));
assert.eq([2, 3, 4, 5], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");
