// tsv1.js

t = new ToolTest("tsv1");

c = t.startDB("foo");

base = {
    a: "",
    b: 1,
    c: "foobar",
    d: 5,
    e: -6
};

t.runTool("import",
          "--file",
          "jstests/tool/data/a.tsv",
          "-d",
          t.baseName,
          "-c",
          "foo",
          "--type",
          "tsv",
          "-f",
          "a,b,c,d,e");
assert.soon("2 == c.count()", "restore 2");

a = c.find().sort({a: 1}).toArray();
delete a[0]._id;
delete a[1]._id;

assert.docEq({a: "a", b: "b", c: "c", d: "d", e: "e"}, a[1], "tsv parse 1");
assert.docEq(base, a[0], "tsv parse 0");

c.drop();
assert.eq(0, c.count(), "after drop 2");

t.runTool("import",
          "--file",
          "jstests/tool/data/a.tsv",
          "-d",
          t.baseName,
          "-c",
          "foo",
          "--type",
          "tsv",
          "--headerline");
assert.soon("c.findOne()", "no data after sleep");
assert.eq(1, c.count(), "after restore 2");

x = c.findOne();
delete x._id;
assert.docEq(base, x, "tsv parse 2");

t.stop();
