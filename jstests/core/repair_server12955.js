
mydb = db.getSisterDB("repair_server12955");
mydb.dropDatabase();

mydb.foo.ensureIndex({a: "text"});
mydb.foo.insert({a: "hello world"});

before = mydb.stats().dataFileVersion;

mydb.repairDatabase();

after = mydb.stats().dataFileVersion;

assert.eq(before, after);
mydb.dropDatabase();
