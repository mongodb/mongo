// SERVER-24127 test

var dbTest = db.getSisterDB("DB_capped_server24127");
dbTest.dropDatabase();

assert(dbTest.getCollectionNames().length == 0);

// This create collection call should fail.
var n=NumberLong("9223372036854775807");
var res = dbTest.createCollection("broken", {capped: true, size: n});
assert.eq(false, res.ok);

var n = new NumberLong(9223372036854776000, 2147483647, 4294967295);
var res = dbTest.createCollection("broken", {capped: true, size: n});
assert.eq(false, res.ok);
