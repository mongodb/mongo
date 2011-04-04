// compact.js

db.dropDatabase();

t = db.compacttest;
t.drop();
t.insert({});

print("1");

assert(db.runCommand({ compact: 'compacttest', dev: true }).ok);
assert(t.count() == 1);
var v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(v.deletedCount == 1);
assert(t.getIndexes().length == 1);

print("2");

// works on an empty collection?
t.remove({});
assert(db.runCommand({ compact: 'compacttest', dev: true }).ok);
assert(t.count() == 0);
v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(t.getIndexes().length == 1);
