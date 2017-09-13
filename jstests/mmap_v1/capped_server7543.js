
mydb = db.getSisterDB("capped_server7543");
mydb.dropDatabase();

mydb.createCollection("foo", {capped: true, size: 12288});

assert.eq(12288, mydb.foo.stats().storageSize);
assert.eq(1, mydb.foo.validate(true).extentCount);

mydb.dropDatabase();
