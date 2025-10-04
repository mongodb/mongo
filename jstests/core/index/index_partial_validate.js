let t = db.index_partial_validate;
t.drop();

let res = t.createIndex({a: 1}, {partialFilterExpression: {a: {$lte: 1}}});
assert.commandWorked(res);

res = t.createIndex({b: 1});
assert.commandWorked(res);

res = t.insert({non_indexed_field: "x"});
assert.commandWorked(res);

res = t.validate({full: true});
assert.commandWorked(res);
assert(res.valid, "Validate failed with response:\n" + tojson(res));
