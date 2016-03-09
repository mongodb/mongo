// Test cloning of capped collections

baseName = "jstests_repl_repl8";

rt = new ReplTest("repl8tests");

m = rt.start(true);

m.getDB(baseName).createCollection("first", {capped: true, size: 1000});
assert(m.getDB(baseName).getCollection("first").isCapped());

s = rt.start(false);

assert.soon(function() {
    return s.getDB(baseName).getCollection("first").isCapped();
});

m.getDB(baseName).createCollection("second", {capped: true, size: 1000});
assert.soon(function() {
    return s.getDB(baseName).getCollection("second").isCapped();
});

m.getDB(baseName).getCollection("third").save({a: 1});
assert.soon(function() {
    return s.getDB(baseName).getCollection("third").exists();
});
assert.commandWorked(m.getDB("admin").runCommand(
    {renameCollection: "jstests_repl_repl8.third", to: "jstests_repl_repl8.third_rename"}));
assert(m.getDB(baseName).getCollection("third_rename").exists());
assert(!m.getDB(baseName).getCollection("third").exists());
assert.soon(function() {
    return s.getDB(baseName).getCollection("third_rename").exists();
});
assert.soon(function() {
    return !s.getDB(baseName).getCollection("third").exists();
});

m.getDB(baseName).getCollection("fourth").save({a: 1});
assert.commandWorked(m.getDB(baseName).getCollection("fourth").convertToCapped(1000));
assert(m.getDB(baseName).getCollection("fourth").isCapped());
assert.soon(function() {
    return s.getDB(baseName).getCollection("fourth").isCapped();
});
