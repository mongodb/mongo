// Test dataSize command, when called on the same or different database
// than the collection being queried.
//
// @tags: [
//   #  The test runs commands that are not allowed with security token: dataSize.
//   not_allowed_with_signed_security_token,
//   # Cannot implicitly shard accessed collections because the "dataSize" command returns an
//   # "keyPattern must equal shard key" error response.
//   assumes_unsharded_collection,
//   no_selinux
// ]

var coll = db.foo;
var adminDB = db.getSiblingDB('admin');
coll.drop();

var N = 1000;
for (var i = 0; i < N; i++) {
    coll.insert({_id: i, s: "asdasdasdasdasdasdasd"});
}

var dataSizeCommand =
    {"dataSize": "test.foo", "keyPattern": {"_id": 1}, "min": {"_id": 0}, "max": {"_id": N}};

assert.eq(N,
          db.runCommand(dataSizeCommand).numObjects,
          "dataSize command on 'test.foo' failed when called on the 'test' DB.");
assert.eq(N,
          adminDB.runCommand(dataSizeCommand).numObjects,
          "dataSize command on 'test.foo' failed when called on the 'admin' DB.");

dataSizeCommand.maxObjects = 100;
assert.eq(101,
          db.runCommand(dataSizeCommand).numObjects,
          "dataSize command with max number of objects set failed on 'test' DB");
assert.eq(101,
          db.runCommand(dataSizeCommand).numObjects,
          "dataSize command with max number of objects set failed on 'admin' DB");
