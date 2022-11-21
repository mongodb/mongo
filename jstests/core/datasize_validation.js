// Cannot implicitly shard accessed collections because the "dataSize" command returns an
// "keyPattern must equal shard key" error response.
// The test runs commands that are not allowed with security token: dataSize.
// @tags: [
//   not_allowed_with_security_token,assumes_unsharded_collection, requires_fcv_53]

//
// Test argument validation for dataSize command
//

(function() {
let coll = db.datasize_validation;
coll.drop();
coll.insertOne({_id: 1});
coll.insertOne({_id: 2});
coll.insertOne({_id: 3});
coll.insertOne({_id: 4});

assert.commandFailed(db.runCommand({
    'dataSize': coll.getFullName(),
    min: NumberLong("1"),
    max: NumberLong("2"),
    estimate: false
}),
                     "min and max should be objects");

assert.commandFailed(
    db.runCommand({'dataSize': coll.getFullName(), min: {_id: NumberLong("1")}, estimate: false}),
    "min and max should both be present");
assert.commandFailed(
    db.runCommand({'dataSize': coll.getFullName(), max: {_id: NumberLong("2")}, estimate: false}),
    "min and max should both be present");

let resultWithKey = assert.commandWorked(db.runCommand({
    'dataSize': coll.getFullName(),
    keyPattern: {_id: 1},
    min: {_id: NumberLong("1")},
    max: {_id: NumberLong("2")},
    estimate: false
}));
assert.eq(1, resultWithKey.numObjects, "only 1 object should be inspected between min/max bounds.");

let result = assert.commandWorked(db.runCommand({
    'dataSize': coll.getFullName(),
    min: {_id: NumberLong("1")},
    max: {_id: NumberLong("2")},
    estimate: false
}));
assert.eq(result.size,
          resultWithKey.size,
          "measured size should be equal after keyPattern properly inferred from min/max bounds.");
assert.eq(result.numObjects,
          resultWithKey.numObjects,
          "numObjects should be equal after keyPattern properly inferred from min/max bounds.");
})();
