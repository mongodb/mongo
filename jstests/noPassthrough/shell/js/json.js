
import {describe, it, runTests} from "jstests/libs/mochalite.js";

globalThis.TestData ??= {};

describe("tojson", function() {
    it("should indent and lint properly", function() {
        const obj = ["foo", "bar"];
        let json;

        json = tojson(obj);
        assert.eq(json, '[ "foo", "bar" ]');

        json = tojson(obj, "", true);
        assert.eq(json, '[ "foo", "bar" ]');

        json = tojson(obj, "", false);
        assert.eq(json, '[\n\t"foo",\n\t"bar"\n]');

        json = tojson(obj, "\t", false);
        assert.eq(json, '[\n\t\t"foo",\n\t\t"bar"\n\t]');

        json = tojson(obj, "X", false);
        assert.eq(json, '[\nX\t"foo",\nX\t"bar"\n\t]');

        // multiple chars aren't typical, but this captures current behavior
        json = tojson(obj, "ABC", false);
        assert.eq(json, '[\nABC\t"foo",\nABC\t"bar"\nBC\t]');

        // indents are ignored if linting is off
        json = tojson(obj, "baz", true);
        assert.eq(json, '[ "foo", "bar" ]');
    });

    it("should parse each type properly", function() {
        const obj = {"x": null, "y": true, "z": 123, "w": "foo", "a": undefined};
        let json;

        json = tojson(obj);
        assert.eq(json, '{ "x" : null, "y" : true, "z" : 123, "w" : "foo", "a" : undefined }');

        json = tojson(obj, "", false);
        assert.eq(json,
                  `{
	"x" : null,
	"y" : true,
	"z" : 123,
	"w" : "foo",
	"a" : undefined
}`);

        json = toJsonForLog(obj);
        assert.eq(json, '{"x":null,"y":true,"z":123,"w":"foo","a":{"$undefined":true}}');
    });

    it("parses nulls", function() {
        const obj = {"x": null};
        assert.eq(tojson(obj), '{ "x" : null }');
    });

    it("accepts 'false' indent", function() {
        // treats it like an empty string
        let a = {"foo": 1};
        let json = tojson(a, false, true);
        assert.eq(json, '{ "foo" : 1 }');
    });

    describe("nested", function() {
        it("shallow", function() {
            const obj = {"x": [], "y": {}};
            let json;

            json = tojson(obj);
            assert.eq(tojson(obj), '{ "x" : [ ], "y" : { } }');

            json = tojson(obj, "", true);
            assert.eq(tojson(obj), '{ "x" : [ ], "y" : { } }');

            json = toJsonForLog(obj);
            assert.eq(tojson(obj), '{ "x" : [ ], "y" : { } }');

            json = tojson(obj, "", false);
            assert.eq(tojson(obj), '{ "x" : [ ], "y" : { } }');
        });

        it("deep", function() {
            const obj = {"x": [{"x": [1, 2, []], "z": "ok", "y": [[]]}, {"foo": "bar"}], "y": null};
            let json;

            json = tojson(obj, "", true);
            assert.eq(
                json,
                '{ "x" : [ { "x" : [ 1, 2, [ ] ], "z" : "ok", "y" : [ [ ] ] }, { "foo" : "bar" } ], "y" : null }');

            json = toJsonForLog(obj);
            assert.eq(json, '{"x":[{"x":[1,2,[]],"z":"ok","y":[[]]},{"foo":"bar"}],"y":null}');

            json = tojson(obj, "", false);
            assert.eq(json,
                      `{
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
}`);
        });
    });

    describe("timestamp", function() {
        it("should parse timestamp properly", function() {
            const obj = Timestamp(10, 2);
            let json;

            json = tojson(obj);
            assert.eq(json, "Timestamp(10, 2)");

            json = toJsonForLog(obj);
            assert.eq(json, '{"$timestamp":{"t":10,"i":2}}');
        });
    });

    describe("map", function() {
        it("should parse map properly", function() {
            const obj = new Map([["one", 1], [2, "two"]]);
            let json;

            json = tojson(obj);
            assert.eq(json, 'new Map([ [ "one", 1 ], [ 2, "two" ] ])');

            json = toJsonForLog(obj);
            assert.eq(json, '{"$map":[["one",1],[2,"two"]]}');
        });
    });

    describe("special types", function() {
        it("ObjectId", function() {
            const obj = {
                "x": ObjectId("4ad35a73d2e34eb4fc43579a"),
            };
            let json;

            json = tojson(obj);
            assert.eq(json, '{ "x" : ObjectId("4ad35a73d2e34eb4fc43579a") }');
        });

        it("RegEx", function() {
            const obj = {"y": /xd?/ig};
            let json;

            json = tojson(obj);
            assert.eq(json, '{ "y" : /xd?/gi }');
        });

        it("NumberLong", function() {
            const obj = {x: NumberLong(64)};

            let json = tostrictjson(obj);
            assert.eq(json, '{ "x" : { "$numberLong" : "64" } }');
        });

        it("undefined", function() {
            const obj = undefined;

            let json = tojson(obj);
            assert.eq(json, 'undefined');

            json = toJsonForLog(obj);
            assert.eq(json, '{"$undefined":true}');
        });
    });

    describe("JSON.stringify", function() {
        it("binary", function() {
            const x = BinData(0, "VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==");
            let json;

            json = JSON.stringify(x);
            assert.eq(
                json,
                '{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"}');

            json = toJsonForLog(x);
            assert.eq(
                json,
                '{"$binary":"VG8gYmUgb3Igbm90IHRvIGJlLi4uIFRoYXQgaXMgdGhlIHF1ZXN0aW9uLg==","$type":"00"}');
        });

        it("timestamp", function() {
            const x = Timestamp(987654321, 0);
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$timestamp":{"t":987654321,"i":0}}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$timestamp":{"t":987654321,"i":0}}');
        });

        it("regex", function() {
            const x = /^acme/i;
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$regex":"^acme","$options":"i"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$regex":"^acme","$options":"i"}');
        });

        it("oid", function() {
            const x = ObjectId("579a70d9e249393f153b5bc1");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$oid":"579a70d9e249393f153b5bc1"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$oid":"579a70d9e249393f153b5bc1"}');
        });

        it("ref", function() {
            const x = DBRef("test", "579a70d9e249393f153b5bc1");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$ref":"test","$id":"579a70d9e249393f153b5bc1"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$ref":"test","$id":"579a70d9e249393f153b5bc1"}');
        });

        it("pointer", function() {
            const x = DBPointer("test", ObjectId("579a70d9e249393f153b5bc1"));
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}}');

            json = toJsonForLog(x);
            assert.eq(json, '{"ns":"test","id":{"$oid":"579a70d9e249393f153b5bc1"}}');
        });

        it("undefined", function() {
            const x = undefined;
            let json;

            json = JSON.stringify(x);
            assert.eq(json, undefined);

            json = toJsonForLog(x);
            assert.eq(json, '{"$undefined":true}');
        });

        it("minkey", function() {
            const x = MinKey;
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$minKey":1}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$minKey":1}');
        });

        it("maxkey", function() {
            const x = MaxKey;
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$maxKey":1}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$maxKey":1}');
        });

        it("numberlong", function() {
            const x = NumberLong("12345");
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$numberLong":"12345"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$numberLong":"12345"}');
        });

        it("numberint", function() {
            const x = NumberInt(5);
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '5');

            json = toJsonForLog(x);
            assert.eq(json, '5');
        });

        it("numberdecimal", function() {
            const x = NumberDecimal(3.14);
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '{"$numberDecimal":"3.14000000000000"}');

            json = toJsonForLog(x);
            assert.eq(json, '{"$numberDecimal":"3.14000000000000"}');
        });

        it("date", function() {
            const x = new Date(Date.UTC(1970, 0, 1, 23, 59, 59, 999));
            let json;

            json = JSON.stringify(x);
            assert.eq(json, '"1970-01-01T23:59:59.999Z"');

            json = toJsonForLog(x);
            assert.eq(json, '{"$date":"1970-01-01T23:59:59.999+00:00"}');
        });
    });

    describe("recursive objects", function() {
        it("should stringify objects", function() {
            const x = {};
            x.self = x;
            let json;

            json = toJsonForLog(x);
            assert.eq(json, '{"self":"[recursive]"}');
        });
    });

    describe("containers", function() {
        it("should serialize containers", function() {
            const x = {};
            x.array = new Array(1, "two", [3, false]);
            x.set = new Set([1, "two", true]);
            x.map = new Map([["one", 1], [2, {y: [3, 4]}]]);

            assert.docEq(x, eval('(' + tojson(x) + ')'));

            let json = toJsonForLog(x);
            assert.eq(
                json,
                '{"array":[1,"two",[3,false]],"set":{"$set":[1,"two",true]},"map":{"$map":[["one",1],[2,{"y":[3,4]}]]}}');
        });
    });

    describe("Strings to escape", function() {
        it("serializes strings that needs escaping", function() {
            const stringThatNeedsEscaping = 'ho\"la';

            assert.eq('\"ho\\\"la\"', JSON.stringify(stringThatNeedsEscaping));
            assert.eq(tojson(stringThatNeedsEscaping), '\"ho\\\"la\"');
            assert.eq(toJsonForLog(stringThatNeedsEscaping), '\"ho\\\"la\"');

            const obj = {quotes: stringThatNeedsEscaping};
            assert.eq(tojson(obj), '{ "quotes" : "ho\\\"la" }');
        });

        it("serializes strings in errors", function() {
            const stringThatNeedsEscaping = 'ho\"la';

            assert.eq('{}', JSON.stringify(new Error(stringThatNeedsEscaping)));
            assert.eq(tojson(new Error(stringThatNeedsEscaping)), 'new Error(\"ho\\\"la\")');
            assert.eq(toJsonForLog(new Error(stringThatNeedsEscaping)), '{"$error":"ho\\\"la"}');

            assert.eq('{}', JSON.stringify(new SyntaxError(stringThatNeedsEscaping)));
            assert.eq(tojson(new SyntaxError(stringThatNeedsEscaping)),
                      'new SyntaxError(\"ho\\\"la\")');
            assert.eq(toJsonForLog(new SyntaxError(stringThatNeedsEscaping)),
                      '{"$error":"ho\\\"la"}');
        });
    });

    describe("logformat json", function() {
        it("should override indent", function() {
            const oldLogFormat = TestData.logFormat;
            TestData.logFormat = "json";

            const x = {a: 1, b: [2, 3]};
            let json = tojson(x, "\t");
            TestData.logFormat = oldLogFormat;

            assert.eq(json, '{ "a" : 1, "b" : [ 2, 3 ] }');
        });

        it("should not override if nolint is specified", function() {
            const oldLogFormat = TestData.logFormat;
            TestData.logFormat = "json";

            const x = {a: 1, b: [2, 3]};
            let json = tojson(x, "\t", false);
            TestData.logFormat = oldLogFormat;

            assert.eq(json,
                      `{
		"a" : 1,
		"b" : [
			2,
			3
		]
	}`);
        });
    });
});

runTests();
