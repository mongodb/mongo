mydb = db.getSisterDB("repair_server12955");
assert.commandWorked(mydb.dropDatabase());

assert.commandWorked(mydb.foo.ensureIndex({a: "text"}));
assert.writeOK(mydb.foo.insert({a: "hello world"}));

var res = mydb.stats();
assert.commandWorked(res);
before = res.dataFileVersion;

assert.commandWorked(mydb.repairDatabase());

res = mydb.stats();
assert.commandWorked(res);
after = res.dataFileVersion;

assert.eq(before, after);
assert.commandWorked(mydb.dropDatabase());
