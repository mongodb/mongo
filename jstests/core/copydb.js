// Basic tests for the copydb command.  These only test copying from the same server; these do not
// test the ability of copydb to pull a database from another server (with or without auth).

// Test basic command usage.
var db1 = db.getSisterDB("copydb-test-db1");
var db2 = db.getSisterDB("copydb-test-db2");
assert.commandWorked(db1.dropDatabase());
assert.commandWorked(db2.dropDatabase());
assert.writeOK(db1.foo.save({db1: 1}));
assert.commandWorked(db1.foo.ensureIndex({db1: 1}));
assert.eq(1, db1.foo.count(), "A");
assert.eq(0, db2.foo.count(), "B");
assert.commandWorked(db1.copyDatabase(db1._name, db2._name));
assert.eq(1, db1.foo.count(), "C");
assert.eq(1, db2.foo.count(), "D");
assert.eq(db1.foo.getIndexes().length, db2.foo.getIndexes().length);

// Test command input validation.
assert.commandFailed(db1.adminCommand(
    {copydb: 1, fromdb: db1.getName(), todb: "copydb.invalid"}));  // Name can't contain dot.
