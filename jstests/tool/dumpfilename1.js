// dumpfilename1.js

// Test designed to make sure error that dumping a collection with "/" fails

t = new ToolTest("dumpfilename1");

t.startDB("foo");

c = t.db;
assert.writeOK(c.getCollection("df/").insert({a: 3}));
assert(t.runTool("dump", "--out", t.ext) != 0, "dump should fail with non-zero return code");
t.stop();
