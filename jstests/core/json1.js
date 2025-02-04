if (typeof TestData === "undefined") {
    TestData = {};
}

function assertToJson({fn, expectedStr, assertMsg, logFormat = "legacy"}) {
    assert.eq(true, logFormat == "legacy" || logFormat == "json");
    const oldLogFormat = TestData.logFormat;
    try {
        TestData.logFormat = logFormat;
        assert.eq(expectedStr, fn(), assertMsg);
    } finally {
        TestData.logFormat = oldLogFormat;
    }
}

let x = {quotes: "a\"b", nulls: null};
let y;
eval("y = " + tojson(x));
assertToJson({fn: () => tojson(x), expectedStr: tojson(y), assertMsg: "A"});
assert.eq(typeof (x.nulls), typeof (y.nulls), "B");

// each type is parsed properly
x = {
    "x": null,
    "y": true,
    "z": 123,
    "w": "foo",
    "a": undefined
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr:
        '{\n\t"x" : null,\n\t"y" : true,\n\t"z" : 123,\n\t"w" : "foo",\n\t"a" : undefined\n}',
    assertMsg: "C1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr:
        '{\n\t"x" : null,\n\t"y" : true,\n\t"z" : 123,\n\t"w" : "foo",\n\t"a" : undefined\n}',
    assertMsg: "C2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : null, "y" : true, "z" : 123, "w" : "foo", "a" : undefined }',
    assertMsg: "C3",
    logFormat: "json"
});

x = {
    "x": [],
    "y": {}
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : [ ],\n\t"y" : {\n\t\t\n\t}\n}',
    assertMsg: "D1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : [ ],\n\t"y" : {\n\t\t\n\t}\n}',
    assertMsg: "D2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : [ ], "y" : {  } }',
    assertMsg: "D3",
    logFormat: "json"
});

// nested
x = {
    "x": [{"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"}],
    "y": null
};
assertToJson({
    fn: () => tojson(x),
    expectedStr:
        '{\n\t"x" : [\n\t\t{\n\t\t\t"x" : [\n\t\t\t\t1,\n\t\t\t\t2,\n\t\t\t\t[ ]\n\t\t\t],\n\t\t\t"z" : "ok",\n\t\t\t"y" : [\n\t\t\t\t[ ]\n\t\t\t]\n\t\t},\n\t\t{\n\t\t\t"foo" : "bar"\n\t\t}\n\t],\n\t"y" : null\n}',
    assertMsg: "E1"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr:
        '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
    assertMsg: "E2",
    logFormat: "json"
});

// special types
x = {
    "x": ObjectId("4ad35a73d2e34eb4fc43579a"),
    'z': /xd?/ig
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),\n\t"z" : /xd?/gi\n}',
    assertMsg: "F1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),\n\t"z" : /xd?/gi\n}',
    assertMsg: "F2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : ObjectId("4ad35a73d2e34eb4fc43579a"), "z" : /xd?/gi }',
    assertMsg: "F3",
    logFormat: "json"
});

// Timestamp type
x = {
    "x": Timestamp()
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : Timestamp(0, 0)\n}',
    assertMsg: "G1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : Timestamp(0, 0)\n}',
    assertMsg: "G2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : Timestamp(0, 0) }',
    assertMsg: "G3",
    logFormat: "json"
});

// Timestamp type, second
x = {
    "x": Timestamp(10, 2)
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : Timestamp(10, 2)\n}',
    assertMsg: "H1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '{\n\t"x" : Timestamp(10, 2)\n}',
    assertMsg: "H2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : Timestamp(10, 2) }',
    assertMsg: "H3",
    logFormat: "json"
});

// Map type
x = new Map();
assertToJson({fn: () => tojson(x, "", false), expectedStr: '[ ]', assertMsg: "I"});

x = new Map();
x.set("one", 1);
x.set(2, "two");
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: '[\n\t[\n\t\t\"one\",\n\t\t1\n\t],\n\t[\n\t\t2,\n\t\t\"two\"\n\t]\n]',
    assertMsg: "J"
});

x = new Map();
x.set("one", 1);
x.set(2, {y: [3, 4]});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr:
        '[\n\t[\n\t\t\"one\",\n\t\t1\n\t],\n\t[\n\t\t2,\n\t\t{\n\t\t\t\"y\" : [\n\t\t\t\t3,\n\t\t\t\t4\n\t\t\t]\n\t\t}\n\t]\n]',
    assertMsg: "K1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr:
        '[\n\t[\n\t\t\"one\",\n\t\t1\n\t],\n\t[\n\t\t2,\n\t\t{\n\t\t\t\"y\" : [\n\t\t\t\t3,\n\t\t\t\t4\n\t\t\t]\n\t\t}\n\t]\n]',
    assertMsg: "K2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '[ [ \"one\", 1 ], [ 2, { \"y\" : [ 3, 4 ] } ] ]',
    assertMsg: "K3",
    logFormat: "json"
});

assert.eq(x, x);
assert.neq(x, new Map());

y = new Map();
y.set("one", 1);
y.set(2, {y: [3, 4]});
assert.eq(x, y);

// tostrictjson produces proper output
x = {
    "x": NumberLong(64)
};
assertToJson({
    fn: () => tostrictjson(x),
    expectedStr: '{ "x" : { "$numberLong" : "64" } }',
    assertMsg: "unexpected 'tojson()' output"
});
assertToJson({
    fn: () => tostrictjson(x),
    expectedStr: '{ "x" : { "$numberLong" : "64" } }',
    assertMsg: "unexpected 'tojson()' output",
    logFormat: "json"
});

// JSON.stringify produces proper strict JSON
x = {
    "data_binary": BinData(0, "VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg=="),
    "data_timestamp": Timestamp(987654321, 0),
    "data_regex": /^acme/i,
    "data_oid": ObjectId("579a70d9e249393f153b5bc1"),
    "data_ref": DBRef("test", "579a70d9e249393f153b5bc1"),
    "data_undefined": undefined,
    "data_minkey": MinKey,
    "data_maxkey": MaxKey,
    "data_numberlong": NumberLong("12345"),
    "data_numberint": NumberInt(5),
    "data_numberdecimal": NumberDecimal(3.14)
};

assert.eq(
    JSON.stringify(x),
    '{"data_binary":{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"},"data_timestamp":{"$timestamp":{"t":987654321,"i":0}},"data_regex":{"$regex":"^acme","$options":"i"},"data_oid":{"$oid":"579a70d9e249393f153b5bc1"},"data_ref":{"$ref":"test","$id":"579a70d9e249393f153b5bc1"},"data_minkey":{"$minKey":1},"data_maxkey":{"$maxKey":1},"data_numberlong":{"$numberLong":"12345"},"data_numberint":5,"data_numberdecimal":{"$numberDecimal":"3.14000000000000"}}');

// serializing Error instances
const stringThatNeedsEscaping = 'ho\"la';
assert.eq('\"ho\\\"la\"', JSON.stringify(stringThatNeedsEscaping));
assert.eq('\"ho\\\"la\"', tojson(stringThatNeedsEscaping));
assert.eq('{}', JSON.stringify(new Error(stringThatNeedsEscaping)));
assert.eq('Error(\"ho\\\"la\")', tojson(new Error(stringThatNeedsEscaping)));
