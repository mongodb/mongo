// SERVER-12185: Do not allow insertion or update of docs which will fail the
// "parallel indexing of arrays" test
var coll = db.insert_illegal_doc;
coll.drop();
coll.ensureIndex({a: 1, b: 1});

var res;
// test upsert
res = coll.update({}, {_id: 1, a: [1, 2, 3], b: [4, 5, 6]}, true);
assert.writeError(res);
assert.eq(res.getWriteError().code, ErrorCodes.CannotIndexParallelArrays);
assert.eq(0, coll.find().itcount(), "should not be a doc");

// test insert
res = coll.insert({_id: 1, a: [1, 2, 3], b: [4, 5, 6]});
assert.writeError(res);
assert.eq(res.getWriteError().code, ErrorCodes.CannotIndexParallelArrays);
assert.eq(0, coll.find().itcount(), "should not be a doc");

// test update
res = coll.insert({_id: 1});
assert.writeOK(res, "insert failed");
res = coll.update({_id: 1}, {$set: {a: [1, 2, 3], b: [4, 5, 6]}});
assert.writeError(res);
assert.eq(res.getWriteError().code, ErrorCodes.CannotIndexParallelArrays);
assert.eq(undefined, coll.findOne().a, "update should have failed");
