// Test cloneCollection command
var baseName = "jstests_clonecollection";

var fromMongod = MongoRunner.runMongod({bind_ip: "127.0.0.1"});
var toMongod = MongoRunner.runMongod({bind_ip: "127.0.0.1"});
var f = fromMongod.getDB(baseName);
var t = toMongod.getDB(baseName);

for (i = 0; i < 1000; ++i) {
    f.a.save({i: i});
}
assert.eq(1000, f.a.find().count(), "A1");

assert.commandWorked(t.cloneCollection("localhost:" + fromMongod.port, "a"));
assert.eq(1000, t.a.find().count(), "A2");

t.a.drop();

assert.commandWorked(
    t.cloneCollection("localhost:" + fromMongod.port, "a", {i: {$gte: 10, $lt: 20}}));
assert.eq(10, t.a.find().count(), "A3");

t.a.drop();
assert.eq(0, t.a.getIndexes().length, "prep 2");

f.a.ensureIndex({i: 1});
assert.eq(2, f.a.getIndexes().length, "expected index missing");
assert.commandWorked(t.cloneCollection("localhost:" + fromMongod.port, "a"));
if (t.a.getIndexes().length != 2) {
    printjson(t.a.getIndexes());
}
assert.eq(2, t.a.getIndexes().length, "expected index missing");
// Verify index works
x = t.a.find({i: 50}).hint({i: 1}).explain("executionStats");
printjson(x);
assert.eq(1, x.executionStats.nReturned, "verify 1");
assert.eq(
    1, t.a.find({i: 50}).hint({i: 1}).toArray().length, "match length did not match expected");

// Check that capped-ness is preserved on clone
f.a.drop();
t.a.drop();

f.createCollection("a", {capped: true, size: 1000});
assert(f.a.isCapped());
assert.commandWorked(t.cloneCollection("localhost:" + fromMongod.port, "a"));
assert(t.a.isCapped(), "cloned collection not capped");

// Check that cloning to "system.profile" is disallowed.
f.a.drop();
f.system.profile.drop();
assert.commandWorked(f.setProfilingLevel(2));
assert.writeOK(f.a.insert({}));
assert.gt(f.system.profile.count(), 0);
t.system.profile.drop();
assert.commandFailed(t.cloneCollection("localhost:" + fromMongod.port, "system.profile"));

// Check that cloning a view is disallowed.
f.a.drop();
t.a.drop();

assert.commandWorked(f.createCollection("a"));
assert.commandWorked(f.createView("viewA", "a", []));
assert.commandFailedWithCode(t.cloneCollection("localhost:" + fromMongod.port, "viewA"),
                             ErrorCodes.CommandNotSupportedOnView,
                             "cloneCollection on view expected to fail");
