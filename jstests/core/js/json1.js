if (typeof TestData === "undefined") {
    TestData = {};
}

function assertToJson({fn, expectedStr, assertMsg, logFormat = "plain"}) {
    assert.eq(true, logFormat == "plain" || logFormat == "json");
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
    expectedStr: `{
	"x" : null,
	"y" : true,
	"z" : 123,
	"w" : "foo",
	"a" : undefined
}`,
    assertMsg: "C1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : null,
	"y" : true,
	"z" : 123,
	"w" : "foo",
	"a" : undefined
}`,
    assertMsg: "C2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : null, "y" : true, "z" : 123, "w" : "foo", "a" : undefined }',
    assertMsg: "C3",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"x":null,"y":true,"z":123,"w":"foo","a":{"$undefined":true}}',
    assertMsg: "C4"
});
x = {
    "x": [],
    "y": {}
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : [ ],
	"y" : {
		
	}
}`,
    assertMsg: "D1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : [ ],
	"y" : {
		
	}
}`,
    assertMsg: "D2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : [ ], "y" : {  } }',
    assertMsg: "D3",
    logFormat: "json"
});
assertToJson({fn: () => toJsonForLog(x), expectedStr: '{"x":[],"y":{}}', assertMsg: "D4"});

// nested
x = {
    "x": [{"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"}],
    "y": null
};
assertToJson({
    fn: () => tojson(x),
    expectedStr: `{
	"x" : [
		{
			"x" : [
				1,
				2,
				[ ]
			],
			"z" : "ok",
			"y" : [
				[ ]
			]
		},
		{
			"foo" : "bar"
		}
	],
	"y" : null
}`,
    assertMsg: "E1"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr:
        '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
    assertMsg: "E2",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"x":[{"x":[1,2,[]],"z":"ok","y":[[]]},{"foo":"bar"}],"y":null}',
    assertMsg: "E3"
});

// special types
x = {
    "x": ObjectId("4ad35a73d2e34eb4fc43579a"),
    'z': /xd?/ig
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),
	"z" : /xd?/gi
}`,
    assertMsg: "F1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : ObjectId("4ad35a73d2e34eb4fc43579a"),
	"z" : /xd?/gi
}`,
    assertMsg: "F2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : ObjectId("4ad35a73d2e34eb4fc43579a"), "z" : /xd?/gi }',
    assertMsg: "F3",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"x":{"$oid":"4ad35a73d2e34eb4fc43579a"},"z":{"$regex":"xd?","$options":"gi"}}',
    assertMsg: "F4"
});

// Timestamp type
x = {
    "x": Timestamp()
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : Timestamp(0, 0)
}`,
    assertMsg: "G1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : Timestamp(0, 0)
}`,
    assertMsg: "G2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : Timestamp(0, 0) }',
    assertMsg: "G3",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"x":{"$timestamp":{"t":0,"i":0}}}',
    assertMsg: "G4"
});

// Timestamp type, second
x = {
    "x": Timestamp(10, 2)
};
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : Timestamp(10, 2)
}`,
    assertMsg: "H1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `{
	"x" : Timestamp(10, 2)
}`,
    assertMsg: "H2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: '{ "x" : Timestamp(10, 2) }',
    assertMsg: "H3",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"x":{"$timestamp":{"t":10,"i":2}}}',
    assertMsg: "H4"
});

// Map type
x = new Map();
assertToJson({fn: () => tojson(x, "", false), expectedStr: 'new Map([ ])', assertMsg: "I1"});
assertToJson({fn: () => toJsonForLog(x), expectedStr: '{"$map":[]}', assertMsg: "I2"});

x = new Map();
x.set("one", 1);
x.set(2, "two");
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `new Map([
	[
		"one",
		1
	],
	[
		2,
		"two"
	]
])`,
    assertMsg: "J1"
});
assertToJson(
    {fn: () => toJsonForLog(x), expectedStr: '{"$map":[["one",1],[2,"two"]]}', assertMsg: "J2"});

x = new Map();
x.set("one", 1);
x.set(2, {y: [3, 4]});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `new Map([
	[
		"one",
		1
	],
	[
		2,
		{
			"y" : [
				3,
				4
			]
		}
	]
])`,
    assertMsg: "K1"
});
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `new Map([
	[
		"one",
		1
	],
	[
		2,
		{
			"y" : [
				3,
				4
			]
		}
	]
])`,
    assertMsg: "K2",
    logFormat: "json"
});
assertToJson({
    fn: () => tojson(x),
    expectedStr: 'new Map([ [ "one", 1 ], [ 2, { "y" : [ 3, 4 ] } ] ])',
    assertMsg: "K3",
    logFormat: "json"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"$map":[["one",1],[2,{"y":[3,4]}]]}',
    assertMsg: "K4"
});

assert.eq(x, x);
assert.neq(x, new Map());
assert.docEq(x, eval('(' + tojson(x) + ')'));

y = new Map();
y.set("one", 1);
y.set(2, {y: [3, 4]});
assert.eq(x, y);

// Set type
x = new Set([{"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"}]);
assert.docEq(x, eval('(' + tojson(x) + ')'));
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `new Set([
	{
		"x" : [
			1,
			2,
			[ ]
		],
		"z" : "ok",
		"y" : [
			[ ]
		]
	},
	{
		"foo" : "bar"
	}
])`,
    assertMsg: "L1"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '{"$set":[{"x":[1,2,[]],"z":"ok","y":[[]]},{"foo":"bar"}]}',
    assertMsg: "L2"
});

// Array type
x = new Array({"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"});
assert.docEq(x, eval('(' + tojson(x) + ')'));
assertToJson({
    fn: () => tojson(x, "", false),
    expectedStr: `[
	{
		"x" : [
			1,
			2,
			[ ]
		],
		"z" : "ok",
		"y" : [
			[ ]
		]
	},
	{
		"foo" : "bar"
	}
]`,
    assertMsg: "M1"
});
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr: '[{"x":[1,2,[]],"z":"ok","y":[[]]},{"foo":"bar"}]',
    assertMsg: "M2"
});

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
    "data_pointer": DBPointer("test", ObjectId("579a70d9e249393f153b5bc1")),
    "data_undefined": undefined,
    "data_minkey": MinKey,
    "data_maxkey": MaxKey,
    "data_numberlong": NumberLong("12345"),
    "data_numberint": NumberInt(5),
    "data_numberdecimal": NumberDecimal(3.14),
    "data_date": new Date(Date.UTC(1970, 0, 1, 23, 59, 59, 999))
};

assert.eq(
    JSON.stringify(x),
    '{"data_binary":{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"},"data_timestamp":{"$timestamp":{"t":987654321,"i":0}},"data_regex":{"$regex":"^acme","$options":"i"},"data_oid":{"$oid":"579a70d9e249393f153b5bc1"},"data_ref":{"$ref":"test","$id":"579a70d9e249393f153b5bc1"},"data_pointer":{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}},"data_minkey":{"$minKey":1},"data_maxkey":{"$maxKey":1},"data_numberlong":{"$numberLong":"12345"},"data_numberint":5,"data_numberdecimal":{"$numberDecimal":"3.14000000000000"},"data_date":"1970-01-01T23:59:59.999Z"}');
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr:
        '{"data_binary":{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"},"data_timestamp":{"$timestamp":{"t":987654321,"i":0}},"data_regex":{"$regex":"^acme","$options":"i"},"data_oid":{"$oid":"579a70d9e249393f153b5bc1"},"data_ref":{"$ref":"test","$id":"579a70d9e249393f153b5bc1"},"data_pointer":{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}},"data_undefined":{"$undefined":true},"data_minkey":{"$minKey":1},"data_maxkey":{"$maxKey":1},"data_numberlong":{"$numberLong":"12345"},"data_numberint":5,"data_numberdecimal":{"$numberDecimal":"3.14000000000000"},"data_date":{"$date":"1970-01-01T23:59:59.999+00:00"}}',
    assertMsg: "N1"
});
// Serialize recursive object
x = {};
x.self = x;
assertToJson({fn: () => toJsonForLog(x), expectedStr: '{"self":"[recursive]"}', assertMsg: "N2"});
// Serialize containers
x = {};
x.array = new Array(1, "two", [3, false]);
x.set = new Set([1, "two", true]);
x.map = new Map([["one", 1], [2, {y: [3, 4]}]]);
assert.docEq(x, eval('(' + tojson(x) + ')'));
assertToJson({
    fn: () => toJsonForLog(x),
    expectedStr:
        '{"array":[1,"two",[3,false]],"set":{"$set":[1,"two",true]},"map":{"$map":[["one",1],[2,{"y":[3,4]}]]}}',
    assertMsg: "N3"
});

// Serialize Error instances
const stringThatNeedsEscaping = 'ho\"la';
assert.eq('\"ho\\\"la\"', JSON.stringify(stringThatNeedsEscaping));
assertToJson(
    {fn: () => tojson(stringThatNeedsEscaping), expectedStr: '\"ho\\\"la\"', assertMsg: "O1"});
assertToJson({
    fn: () => toJsonForLog(stringThatNeedsEscaping),
    expectedStr: '\"ho\\\"la\"',
    assertMsg: "O2"
});
assert.eq('{}', JSON.stringify(new Error(stringThatNeedsEscaping)));
assertToJson({
    fn: () => tojson(new Error(stringThatNeedsEscaping)),
    expectedStr: 'new Error(\"ho\\\"la\")',
    assertMsg: "O3"
});
assertToJson({
    fn: () => toJsonForLog(new Error(stringThatNeedsEscaping)),
    expectedStr: '{"$error":"ho\\\"la"}',
    assertMsg: "O4"
});
assertToJson({
    fn: () => tojson(new SyntaxError(stringThatNeedsEscaping)),
    expectedStr: 'new SyntaxError(\"ho\\\"la\")',
    assertMsg: "O5"
});
assertToJson({
    fn: () => toJsonForLog(new SyntaxError(stringThatNeedsEscaping)),
    expectedStr: '{"$error":"ho\\\"la"}',
    assertMsg: "O6"
});

// Serialize 'undefined'
assertToJson(
    {fn: () => toJsonForLog(undefined), expectedStr: '{"$undefined":true}', assertMsg: "P1"});
assertToJson(
    {fn: () => toJsonForLog([undefined]), expectedStr: '[{"$undefined":true}]', assertMsg: "P2"});
assertToJson({
    fn: () => toJsonForLog({hello: undefined}),
    expectedStr: '{"hello":{"$undefined":true}}',
    assertMsg: "P3"
});
