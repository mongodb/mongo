// compact.js

t = db.compacttest;
t.drop();
t.insert({ x: 3 });
t.insert({ x: 3 });
t.insert({ x: 5 });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.insert({ x: 4, z: 2, k: 'aaa' });
t.ensureIndex({ x: 1 });

print("1");

var res = db.runCommand({ compact: 'compacttest', dev: true, force: true });
printjson(res);
assert(res.ok);
assert(t.count() == 9);
var v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(v.deletedCount == 1);
assert(t.getIndexes().length == 2);
var ssize = t.stats().storageSize;

print("2");
res = db.runCommand({ compact: 'compacttest', dev: true,paddingBytes:1000, force:true });
assert(res.ok);
assert(t.count() == 9);
var v = t.validate(true);
assert(v.ok);
assert(t.stats().storageSize > ssize, "expected more storage given padding is higher. however it rounds off so if something changed this could be");
//printjson(t.stats());

print("z");

t.insert({ x: 4, z: 2, k: { a: "", b: ""} });
t.insert({ x: 4, z: 2, k: { a: "", b: ""} });
t.insert({ x: 4, z: 2, k: { a: "", b: ""} });
t.insert({ x: 4, z: 2, k: { a: "", b: ""} });
t.insert({ x: 4, z: null, k: { f: "", b: ""} });
t.insert({ x: 4, z: null, k: { c: ""} });
t.insert({ x: 4, z: null, k: { h: ""} });
t.insert({ x: 4, z: null });
t.insert({ x: 4, z: 3});
t.insert({ x: 4, z: 2, k: { a: "", b: ""} });
t.insert({ x: 4, z: null, k: { c: ""} });
t.insert({ x: 4, z: null, k: { c: ""} });
t.insert({ x: 4, z: 3, k: { c: ""} });

t.ensureIndex({ z: 1, k: 1 });
//t.ensureIndex({ z: 1, k: 1 }, { unique: true });
//t.ensureIndex({ z: 1, k: 1 }, { dropDups: true, unique:true });

res = db.runCommand({ compact: 'compacttest', dev: true, paddingFactor: 1.2, force:true });
printjson(res);
assert(res.ok);
assert(t.count() > 13);
var v = t.validate(true);
assert(v.ok);

print("3");

// works on an empty collection?
t.remove({});
assert(db.runCommand({ compact: 'compacttest', dev: true, force:true }).ok);
assert(t.count() == 0);
v = t.validate(true);
assert(v.ok);
assert(v.extentCount == 1);
assert(t.getIndexes().length == 3);

