// csvimport1.js

t = new ToolTest("csvimport1");

c = t.startDB("foo");

base = [];
base.push({
    a: 1,
    b: "this is some text.\nThis text spans multiple lines, and just for fun\ncontains a comma",
    "c": "This has leading and trailing whitespace!"
});
base.push({
    a: 2,
    b: "When someone says something you \"put it in quotes\"",
    "c": "I like embedded quotes/slashes\\backslashes"
});
base.push({
    a: 3,
    b: "  This line contains the empty string and has leading and trailing whitespace inside the quotes!  ",
    "c": ""
});
base.push({a: 4, b: "", "c": "How are empty entries handled?"});
base.push({a: 5, b: "\"\"", c: "\"This string is in quotes and contains empty quotes (\"\")\""});
base.push({a: "a", b: "b", c: "c"});

assert.eq(0, c.count(), "setup");

t.runTool("import",
          "--file",
          "jstests/tool/data/csvimport1.csv",
          "-d",
          t.baseName,
          "-c",
          "foo",
          "--type",
          "csv",
          "-f",
          "a,b,c");
assert.soon(base.length + " == c.count()", "after import 1 ");

a = c.find().sort({a: 1}).toArray();
for (i = 0; i < base.length; i++) {
    delete a[i]._id;
    assert.docEq(base[i], a[i], "csv parse " + i);
}

c.drop();
assert.eq(0, c.count(), "after drop");

t.runTool("import",
          "--file",
          "jstests/tool/data/csvimport1.csv",
          "-d",
          t.baseName,
          "-c",
          "foo",
          "--type",
          "csv",
          "--headerline");
assert.soon("c.findOne()", "no data after sleep");
assert.eq(base.length - 1, c.count(), "after import 2");

x = c.find().sort({a: 1}).toArray();
for (i = 0; i < base.length - 1; i++) {
    delete x[i]._id;
    assert.docEq(base[i], x[i], "csv parse with headerline " + i);
}

t.stop();
