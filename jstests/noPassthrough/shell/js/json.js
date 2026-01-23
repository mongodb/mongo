import {after, before, describe, it} from "jstests/libs/mochalite.js";

globalThis.TestData ??= {};

describe("Array.tojson", () => {
    it("should indent and lint arrays properly", () => {
        const arr = ["foo", "bar"];
        let json;

        json = Array.tojson(arr);
        assert.eq(
            json,
            `\
[
	"foo",
	"bar"
]`,
        );

        json = Array.tojson(arr, "", false);
        assert.eq(
            json,
            `\
[
	"foo",
	"bar"
]`,
        );

        json = Array.tojson(arr, "", true);
        assert.eq(json, '[ "foo", "bar" ]');

        json = Array.tojson(arr, " ", true);
        assert.eq(json, '[ "foo", "bar" ]');
    });
});

describe("tojson", function () {
    it("should indent and lint arrays properly", function () {
        const arr = ["foo", "bar"];
        let json;

        json = tojson(arr);
        assert.eq(json, '[ "foo", "bar" ]');

        json = tojson(arr, "", true);
        assert.eq(json, '[ "foo", "bar" ]');

        json = tojson(arr, "", false);
        assert.eq(
            json,
            `\
[
	"foo",
	"bar"
]`,
        );

        json = tojson(arr, "\t", false);
        assert.eq(
            json,
            `\
[
	"foo",
	"bar"
]`,
        );

        json = tojson(arr, "X", false);
        assert.eq(
            json,
            `\
[
X"foo",
X"bar"
]`,
        );

        // multiple chars aren't typical, but this captures current behavior
        json = tojson(arr, "ABC", false);
        assert.eq(
            json,
            `\
[
ABC"foo",
ABC"bar"
]`,
        );

        // indents are ignored if linting is off
        json = tojson(arr, "baz", true);
        assert.eq(json, '[ "foo", "bar" ]');
    });

    it("should parse each type properly", function () {
        const obj = {"x": null, "y": true, "z": 123, "w": "foo", "a": undefined};
        let json;

        json = tojson(obj);
        assert.eq(json, '{ "x" : null, "y" : true, "z" : 123, "w" : "foo", "a" : undefined }');

        json = tojson(obj, "", false);
        assert.eq(
            json,
            `\
{
	"x" : null,
	"y" : true,
	"z" : 123,
	"w" : "foo",
	"a" : undefined
}`,
        );

        json = toJsonForLog(obj);
        assert.eq(json, '{"x":null,"y":true,"z":123,"w":"foo","a":{"$undefined":true}}');
    });

    it("makes short objects a oneliner", function () {
        const obj = {"x": 1, "y": 2};
        let json;

        // this should be a multiline, but gets shortened because it can fit on one line
        json = tojson(obj);
        assert.eq(json, '{ "x" : 1, "y" : 2 }');

        json = tojson(obj, "", undefined); // implicitly the same as above
        assert.eq(json, '{ "x" : 1, "y" : 2 }');

        json = tojson(obj, "", true); // explicitly a oneliner
        assert.eq(json, '{ "x" : 1, "y" : 2 }');

        json = tojson(obj, " ", true); // matches tojsononeline calls
        assert.eq(json, '{ "x" : 1, "y" : 2 }');

        json = tojson(obj, "", false); // force pretty print
        assert.eq(
            json,
            `\
{
	"x" : 1,
	"y" : 2
}`,
        );

        const longZ = "z".repeat(80);
        obj.z = longZ;
        json = tojson(obj);
        // uses default indent of "\t" with pretty printing
        assert.eq(
            json,
            `\
{
	"x" : 1,
	"y" : 2,
	"z" : "${longZ}"
}`,
        );
    });

    it("parses nulls", function () {
        const obj = {"x": null};
        assert.eq(tojson(obj), '{ "x" : null }');
    });

    it("accepts 'false' indent", function () {
        // treats it like an empty string
        let a = {"foo": 1};
        let json = tojson(a, false, true);
        assert.eq(json, '{ "foo" : 1 }');
    });

    describe("nested", function () {
        it("shallow", function () {
            const obj = {"x": [], "y": {}};
            let json;

            json = tojson(obj);
            assert.eq(json, '{ "x" : [ ], "y" : { } }');

            json = tojson(obj, "", true);
            assert.eq(json, '{ "x" : [ ], "y" : { } }');

            json = tojson(obj, " ", true);
            assert.eq(json, '{ "x" : [ ], "y" : { } }');

            json = toJsonForLog(obj);
            assert.eq(json, '{"x":[],"y":{}}');

            json = tojson(obj, "", false);
            assert.eq(
                json,
                `\
{
	"x" : [ ],
	"y" : {
		
	}
}`,
            );

            let x = {"a": {"x": "foobar"}};

            json = tojson(x, " ", true);
            assert.eq(json, '{ "a" : { "x" : "foobar" } }');
        });

        it("deep", function () {
            const obj = {"x": [{"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"}], "y": null};
            let json;

            json = tojson(obj, "", true);
            assert.eq(
                json,
                '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
            );

            json = tojson(obj, " ", true); // matches tojsononeline calls
            assert.eq(
                json,
                '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
            );

            json = toJsonForLog(obj);
            assert.eq(json, '{"x":[{"x":[1,2,[]],"z":"ok","y":[[]]},{"foo":"bar"}],"y":null}');

            json = tojson(obj, "", false);
            assert.eq(
                json,
                `\
{
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
            );

            json = tojson(obj, " ", false);
            assert.eq(
                json,
                `\
{
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
            );

            json = tojson(obj, "  ", false);
            assert.eq(
                json,
                `\
{
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
            );

            json = tojson(obj, "ABC", false);
            assert.eq(
                json,
                `\
{
ABC"x" : [
ABCABC{
ABCABCABC"x" : [
ABCABCABCABC1,
ABCABCABCABC2,
ABCABCABCABC[ ]
ABCABCABC],
ABCABCABC"z" : "ok",
ABCABCABC"y" : [
ABCABCABCABC[ ]
ABCABCABC]
ABCABC},
ABCABC{
ABCABCABC"foo" : "bar"
ABCABC}
ABC],
ABC"y" : null
}`,
            );

            json = tojson(obj, "ABC", true); // indents ignored completely
            assert.eq(
                json,
                '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
            );

            json = tojson(obj, "ABC", true); // indents are ignored completely
            assert.eq(
                json,
                '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }',
            );
        });
    });

    describe("timestamp", function () {
        it("should parse timestamp properly", function () {
            const obj = Timestamp(10, 2);
            let json;

            json = tojson(obj);
            assert.eq(json, "Timestamp(10, 2)");

            json = toJsonForLog(obj);
            assert.eq(json, '{"$timestamp":{"t":10,"i":2}}');
        });
    });

    describe("map", function () {
        it("should parse map properly", function () {
            const obj = new Map([
                ["one", 1],
                [2, "two"],
            ]);
            let json;

            json = tojson(obj);
            assert.eq(json, 'new Map([ [ "one", 1 ], [ 2, "two" ] ])');

            json = toJsonForLog(obj);
            assert.eq(json, '{"$map":[["one",1],[2,"two"]]}');
        });
    });

    describe("special types", function () {
        it("ObjectId", function () {
            const obj = {
                "x": ObjectId("4ad35a73d2e34eb4fc43579a"),
            };
            let json;

            json = tojson(obj);
            assert.eq(json, '{ "x" : ObjectId("4ad35a73d2e34eb4fc43579a") }');
        });

        it("RegEx", function () {
            const obj = {"y": /xd?/gi};
            let json;

            json = tojson(obj);
            assert.eq(json, '{ "y" : /xd?/gi }');
        });

        it("NumberLong", function () {
            const obj = {x: NumberLong(64)};

            let json = tostrictjson(obj);
            assert.eq(json, '{ "x" : { "$numberLong" : "64" } }');
        });

        it("undefined", function () {
            const obj = undefined;

            let json = tojson(obj);
            assert.eq(json, "undefined");

            json = toJsonForLog(obj);
            assert.eq(json, '{"$undefined":true}');
        });
    });

    describe("JSON.stringify", function () {
        it("binary", function () {
            const x = BinData(0, "VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"}');
        });

        it("regex", function () {
            const x = /^acme/i;
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$regex":"^acme","$options":"i"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$regex":"^acme","$options":"i"}');
        });

        it("oid", function () {
            const x = ObjectId("579a70d9e249393f153b5bc1");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$oid":"579a70d9e249393f153b5bc1"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$oid":"579a70d9e249393f153b5bc1"}');
        });

        it("ref", function () {
            const x = DBRef("test", "579a70d9e249393f153b5bc1");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$ref":"test","$id":"579a70d9e249393f153b5bc1"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$ref":"test","$id":"579a70d9e249393f153b5bc1"}');
        });

        it("pointer", function () {
            const x = DBPointer("test", ObjectId("579a70d9e249393f153b5bc1"));
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}}');

            json = toJsonForLog(x);
            assert.eq(json, '{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}}');
        });

        it("undefined", function () {
            const x = undefined;
            let json;

            json = tojson(x);
            assert.eq(json, "undefined");

            json = JSON.stringify(x);
            assert.eq(json, undefined);

            json = toJsonForLog(x);
            assert.eq(json, '{"$undefined":true}');
        });

        it("minkey", function () {
            const x = MinKey;
            let json;

            json = tojson(x);
            assert.eq(json, '{ "$minKey" : 1 }');

            json = JSON.stringify(x);
            assert.eq(json, '{"$minKey":1}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$minKey":1}');
        });

        it("maxkey", function () {
            const x = MaxKey;
            let json;

            json = tojson(x);
            assert.eq(json, '{ "$maxKey" : 1 }');

            json = JSON.stringify(x);
            assert.eq(json, '{"$maxKey":1}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$maxKey":1}');
        });

        it("numberlong", function () {
            const x = NumberLong("12345");
            let json;

            json = tojson(x);
            assert.eq(json, "NumberLong(12345)");

            json = JSON.stringify(x);
            assert.eq(json, '{"$numberLong":"12345"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$numberLong":"12345"}');
        });

        it("numberint", function () {
            const x = NumberInt(5);
            let json;

            json = tojson(x);
            assert.eq(json, "NumberInt(5)");

            json = JSON.stringify(x);
            assert.eq(json, "5");

            json = toJsonForLog(x);
            assert.eq(json, "5");
        });

        it("numberdecimal", function () {
            const x = NumberDecimal(3.14);
            let json;

            json = tojson(x);
            assert.eq(json, 'NumberDecimal("3.14000000000000")');

            json = JSON.stringify(x);
            assert.eq(json, '{"$numberDecimal":"3.14000000000000"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$numberDecimal":"3.14000000000000"}');
        });
    });

    describe("recursive objects", function () {
        it("should stringify objects", function () {
            const x = {};
            x.self = x;
            let json;

            json = toJsonForLog(x);
            assert.eq(json, '{"self":"[recursive]"}');
        });
    });

    describe("containers", function () {
        it("should serialize containers", function () {
            const x = {};
            x.array = new Array(1, "two", [3, false]);
            x.set = new Set([1, "two", true]);
            x.map = new Map([
                ["one", 1],
                [2, {y: [3, 4]}],
            ]);

            let json = tojson(x);
            assert.eq(
                json,
                `\
{
	"array" : [
		1,
		"two",
		[
			3,
			false
		]
	],
	"set" : new Set([
		1,
		"two",
		true
	]),
	"map" : new Map([
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
	])
}`,
            );

            assert.docEq(x, eval("(" + tojson(x) + ")"));

            json = toJsonForLog(x);
            assert.eq(
                json,
                '{"array":[1,"two",[3,false]],"set":{"$set":[1,"two",true]},"map":{"$map":[["one",1],[2,{"y":[3,4]}]]}}',
            );
        });
    });

    describe("Strings to escape", function () {
        it("serializes strings that needs escaping", function () {
            const stringThatNeedsEscaping = 'ho"la';

            assert.eq('"ho\\"la"', JSON.stringify(stringThatNeedsEscaping));
            assert.eq(tojson(stringThatNeedsEscaping), '"ho\\"la"');
            assert.eq(toJsonForLog(stringThatNeedsEscaping), '"ho\\"la"');

            const obj = {quotes: stringThatNeedsEscaping};
            assert.eq(tojson(obj), '{ "quotes" : "ho\\"la" }');
        });

        it("serializes strings in errors", function () {
            const stringThatNeedsEscaping = 'ho"la';

            assert.eq("{}", JSON.stringify(new Error(stringThatNeedsEscaping)));
            assert.eq(tojson(new Error(stringThatNeedsEscaping)), 'new Error("ho\\"la")');
            assert.eq(toJsonForLog(new Error(stringThatNeedsEscaping)), '{"$error":"ho\\"la"}');

            assert.eq("{}", JSON.stringify(new SyntaxError(stringThatNeedsEscaping)));
            assert.eq(tojson(new SyntaxError(stringThatNeedsEscaping)), 'new SyntaxError("ho\\"la")');
            assert.eq(toJsonForLog(new SyntaxError(stringThatNeedsEscaping)), '{"$error":"ho\\"la"}');
        });
    });

    describe("logformat json", function () {
        const oldLogFormat = TestData.logFormat;
        before(() => {
            TestData.logFormat = "json";
        });
        after(() => {
            TestData.logFormat = oldLogFormat;
        });

        it("should override indent", function () {
            const x = {a: 1, b: [2, 3]};
            let json = tojson(x, "\t");

            assert.eq(json, '{ "a" : 1, "b" : [ 2, 3 ] }');
        });

        it("should not override if nolint is specified", function () {
            const x = {a: 1, b: [2, 3]};
            let json = tojson(x, "\t", false);

            assert.eq(
                json,
                `\
{
	"a" : 1,
	"b" : [
		2,
		3
	]
}`,
            );
        });
    });
});

describe("tojsonObject", () => {
    it("empty object", () => {
        assert.eq(
            tojsonObject({}, "", false),
            `\
{
	
}`,
        );
        assert.eq(tojsonObject({}, "", true), "{ }");
        assert.eq(
            tojsonObject({}, "\t\t", false),
            `\
{
		
}`,
        );
    });

    it("single field", () => {
        assert.eq(
            tojsonObject({a: 1}, "", false),
            `\
{
	"a" : 1
}`,
        );
        assert.eq(tojsonObject({a: 1}, "", true), '{ "a" : 1 }');
        assert.eq(
            tojsonObject({a: 1}, "\t\t", false),
            `\
{
		"a" : 1
}`,
        );
    });

    it("multiple fields", () => {
        assert.eq(
            tojsonObject({a: 1, b: 2}, "", false),
            `\
{
	"a" : 1,
	"b" : 2
}`,
        );
        assert.eq(tojsonObject({a: 1, b: 2}, "", true), '{ "a" : 1, "b" : 2 }');
        assert.eq(
            tojsonObject({a: 1, b: 2}, "\t\t", false),
            `\
{
		"a" : 1,
		"b" : 2
}`,
        );
    });

    it("nested fields", () => {
        assert.eq(
            tojsonObject({a: 1, b: {bb: 2, cc: 3}}, "", false),
            `\
{
	"a" : 1,
	"b" : {
		"bb" : 2,
		"cc" : 3
	}
}`,
        );
        assert.eq(tojsonObject({a: 1, b: {bb: 2, cc: 3}}, "", true), '{ "a" : 1, "b" : { "bb" : 2, "cc" : 3 } }');
        assert.eq(
            tojsonObject({a: 1, b: {bb: 2, cc: 3}}, "\t\t", false),
            `\
{
		"a" : 1,
		"b" : {
				"bb" : 2,
				"cc" : 3
		}
}`,
        );
    });
});

// The "depth" is not really a user-facing value to tune; it is more for tracking internal recursion limits.
// It doesn't help truncate the depth itself, but adjusting the tojson.MAX_DEPTH does.
describe("depth", () => {
    const obj = {a: {b: {c: {d: {e: 5}}}}};
    let json;

    it("does not truncate", () => {
        // does not truncate because we still have MAX_DEPTH-2 stacks to go,
        // but it does indent an extra 2 levels
        json = tojson(obj, "", false, 2);
        assert.eq(
            json,
            `\
{
			"a" : {
				"b" : {
					"c" : {
						"d" : {
							"e" : 5
						}
					}
				}
			}
		}`,
        );

        json = tojson(obj, " ", false, 2);
        assert.eq(
            json,
            `\
{
   "a" : {
    "b" : {
     "c" : {
      "d" : {
       "e" : 5
      }
     }
    }
   }
  }`,
        );
    });

    describe("MAX_DEPTH", () => {
        const oldDepth = tojson.MAX_DEPTH;
        before(() => {
            tojson.MAX_DEPTH = 2;
        });
        after(() => {
            tojson.MAX_DEPTH = oldDepth;
        });

        it("truncates", () => {
            json = tojson(obj, "", false);
            assert.eq(
                json,
                `\
{
	"a" : {
		"b" : {
			"c" : [Object]
		}
	}
}`,
            );

            json = tojson(obj, " ", false);
            assert.eq(
                json,
                `\
{
 "a" : {
  "b" : {
   "c" : [Object]
  }
 }
}`,
            );

            json = tojson(obj, "", false);
            assert.eq(
                json,
                `\
{
	"a" : {
		"b" : {
			"c" : [Object]
		}
	}
}`,
            );

            json = tojson(obj, " ", false);
            assert.eq(
                json,
                `\
{
 "a" : {
  "b" : {
   "c" : [Object]
  }
 }
}`,
            );
        });
    });
});
