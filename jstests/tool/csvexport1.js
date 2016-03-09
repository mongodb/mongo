// csvexport1.js

t = new ToolTest("csvexport1");

c = t.startDB("foo");

assert.eq(0, c.count(), "setup1");

objId = ObjectId();

c.insert({a: new NumberInt(1), b: objId, c: [1, 2, 3], d: {a: "hello", b: "world"}, e: '-'});
c.insert({a: -2.0, c: MinKey, d: "Then he said, \"Hello World!\"", e: new NumberLong(3)});
// clang-format off
c.insert({ a : new BinData(0, "1234"), b : ISODate("2009-08-27T12:34:56.789"),
           c : new Timestamp(1234, 9876), d : /foo*\"bar\"/i,
           e : function foo() { print("Hello World!"); }});
// clang-format on

assert.eq(3, c.count(), "setup2");

t.runTool("export", "--out", t.extFile, "-d", t.baseName, "-c", "foo", "--csv", "-f", "a,b,c,d,e");

c.drop();

assert.eq(0, c.count(), "after drop");

t.runTool(
    "import", "--file", t.extFile, "-d", t.baseName, "-c", "foo", "--type", "csv", "--headerline");

assert.soon(3 + " == c.count()", "after import");

// Note: Exporting and Importing to/from CSV is not designed to be round-trippable
expected = [];
expected.push({
    a: 1,
    b: "ObjectId(" + objId.valueOf() + ")",
    c: [1, 2, 3],
    d: {"a": "hello", "b": "world"},
    e: "-"
});
expected.push({a: -2.0, b: "", c: "$MinKey", d: "Then he said, \"Hello World!\"", e: 3});
// "t" should be 1234, but the shell interprets the first field of timestamps as milliseconds while
// they are stored as seconds.  See SERVER-7718.
// clang-format off
expected.push({ a : "D76DF8", b : "2009-08-27T12:34:56.789Z",
                c : { "$timestamp" : { "t" : 1234, "i" : 9876 } },
                d : "/foo*\\\"bar\\\"/i", e : tojson(function foo() { print("Hello World!"); })});
// clang-format on

actual = [];
actual.push(c.find({a: 1}).toArray()[0]);
actual.push(c.find({a: -2.0}).toArray()[0]);
actual.push(c.find({a: "D76DF8"}).toArray()[0]);

for (i = 0; i < expected.length; i++) {
    delete actual[i]._id;
    assert.eq(Object.keys(expected[i]).length, Object.keys(actual[i]).length);
    keys = Object.keys(expected[i]);
    for (var j = 0; j < keys.length; j++) {
        expectedVal = expected[i][keys[j]];
        if ((typeof expectedVal) == "object") {
            // For fields which contain arrays or objects, they have been
            // exported as JSON - parse the JSON in the output and verify
            // that it matches the original document's value
            assert.docEq(expectedVal, JSON.parse(actual[i][keys[j]]), "CSV export " + i);
        } else {
            // Otherwise just compare the values directly
            assert.eq(expectedVal, actual[i][keys[j]], "CSV export " + i);
        }
    }
}

t.stop();
