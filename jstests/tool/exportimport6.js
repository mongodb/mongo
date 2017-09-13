// exportimport6.js
// test export with skip, limit and sort

t = new ToolTest("exportimport6");

c = t.startDB("foo");
assert.eq(0, c.count(), "setup1");
c.save({a: 1, b: 1});
c.save({a: 1, b: 2});
c.save({a: 2, b: 3});
c.save({a: 2, b: 3});
c.save({a: 3, b: 4});
c.save({a: 3, b: 5});

assert.eq(6, c.count(), "setup2");

t.runTool("export",
          "--out",
          t.extFile,
          "-d",
          t.baseName,
          "-c",
          "foo",
          "--sort",
          "{a:1, b:-1}",
          "--skip",
          "4",
          "--limit",
          "1");

c.drop();
assert.eq(0, c.count(), "after drop", "-d", t.baseName, "-c", "foo");
t.runTool("import", "--file", t.extFile, "-d", t.baseName, "-c", "foo");
assert.eq(1, c.count(), "count should be 1");
assert.eq(5, c.findOne().b, printjson(c.findOne()));

t.stop();
