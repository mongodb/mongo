// Tries to create too many indexes. Used to fail as SERVER-16673.

var coll = db.index_create_too_many;
coll.drop();

// create 62 indexes, which leaves us with 63 indexes total (+1 for the _id index)
for (var i = 0; i < 62; i++) {
    var name = 'i' + i;
    var spec = {key: {}, name: name};
    spec.key[name] = 1;

    var res = coll.runCommand('createIndexes', {indexes: [spec]});
    assert.commandWorked(res, tojson(res));
}

// attempt to add 2 more indexes to push over the limit (64).
var newSpecs = [{key: {i62: 1}, name: 'i62'}, {key: {i63: 1}, name: 'i63'}];

var res = coll.runCommand('createIndexes', {indexes: newSpecs});
assert.commandFailed(res, tojson(res));
assert.eq(res.code, 67);  // CannotCreateIndex
