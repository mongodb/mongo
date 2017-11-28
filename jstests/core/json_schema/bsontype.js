// @tags: [requires_non_retryable_commands]

/**
 * Tests for the non-standard 'bsonType' keyword in JSON Schema, as well as some tests for 'type'.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.jstests_schema_bsontype;

    // bsonType "double".
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {num: 3}, true);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {num: NumberLong(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {num: NumberInt(3)}, false);
    assertSchemaMatch(
        coll, {properties: {num: {bsonType: "double"}}}, {num: NumberDecimal(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {num: {}}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {num: [3]}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "double"}}}, {foo: {}}, true);

    // type "double" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "double"}}}}).itcount());

    // bsonType "string".
    assertSchemaMatch(coll, {properties: {str: {bsonType: "string"}}}, {str: ""}, true);
    assertSchemaMatch(coll, {properties: {str: {bsonType: "string"}}}, {str: true}, false);
    assertSchemaMatch(coll, {properties: {str: {bsonType: "string"}}}, {str: [1, "foo"]}, false);

    // type "string".
    assertSchemaMatch(coll, {properties: {str: {type: "string"}}}, {str: ""}, true);
    assertSchemaMatch(coll, {properties: {str: {type: "string"}}}, {str: true}, false);
    assertSchemaMatch(coll, {properties: {str: {type: "string"}}}, {str: [1, "foo"]}, false);

    // bsonType "object".
    assertSchemaMatch(coll, {bsonType: "object"}, {}, true);
    assertSchemaMatch(coll, {properties: {obj: {bsonType: "object"}}}, {obj: {}}, true);
    assertSchemaMatch(coll, {properties: {obj: {bsonType: "object"}}}, {obj: true}, false);
    assertSchemaMatch(coll, {properties: {obj: {bsonType: "object"}}}, {obj: [{}]}, false);

    // type "object".
    assertSchemaMatch(coll, {type: "object"}, {}, true);
    assertSchemaMatch(coll, {properties: {obj: {type: "object"}}}, {obj: {}}, true);
    assertSchemaMatch(coll, {properties: {obj: {type: "object"}}}, {obj: true}, false);
    assertSchemaMatch(coll, {properties: {obj: {type: "object"}}}, {obj: [{}]}, false);

    // bsonType "array".
    assertSchemaMatch(coll, {bsonType: "array"}, {arr: []}, false);
    assertSchemaMatch(coll, {properties: {arr: {bsonType: "array"}}}, {arr: []}, true);
    assertSchemaMatch(coll, {properties: {arr: {bsonType: "array"}}}, {arr: {}}, false);

    // type "array".
    assertSchemaMatch(coll, {type: "array"}, {arr: []}, false);
    assertSchemaMatch(coll, {properties: {arr: {type: "array"}}}, {arr: []}, true);
    assertSchemaMatch(coll, {properties: {arr: {type: "array"}}}, {arr: {}}, false);

    // bsonType "binData".
    assertSchemaMatch(coll,
                      {properties: {bin: {bsonType: "binData"}}},
                      {bin: BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")},
                      true);
    assertSchemaMatch(coll, {properties: {bin: {bsonType: "binData"}}}, {bin: {}}, false);

    // type "binData" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {bin: {type: "binData"}}}}).itcount());

    // bsonType "undefined".
    assertSchemaMatch(
        coll, {properties: {u: {bsonType: "undefined"}}, required: ["u"]}, {u: undefined}, true);
    assertSchemaMatch(coll, {properties: {u: {bsonType: "undefined"}}, required: ["u"]}, {}, false);
    assertSchemaMatch(
        coll, {properties: {u: {bsonType: "undefined"}}, required: ["u"]}, {u: null}, false);

    // type "undefined" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {u: {type: "undefined"}}}}).itcount());

    // bsonType "objectId".
    assertSchemaMatch(coll, {properties: {o: {bsonType: "objectId"}}}, {o: ObjectId()}, true);
    assertSchemaMatch(coll, {properties: {o: {bsonType: "objectId"}}}, {o: 1}, false);

    // type "objectId" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {o: {type: "objectId"}}}}).itcount());

    // bsonType "bool".
    assertSchemaMatch(coll, {properties: {b: {bsonType: "bool"}}}, {b: true}, true);
    assertSchemaMatch(coll, {properties: {b: {bsonType: "bool"}}}, {b: false}, true);
    assertSchemaMatch(coll, {properties: {b: {bsonType: "bool"}}}, {b: 1}, false);

    // bsonType "boolean" should fail.
    assert.throws(() =>
                      coll.find({$jsonSchema: {properties: {b: {bsonType: "boolean"}}}}).itcount());

    // type "boolean".
    assertSchemaMatch(coll, {properties: {b: {type: "boolean"}}}, {b: true}, true);
    assertSchemaMatch(coll, {properties: {b: {type: "boolean"}}}, {b: false}, true);
    assertSchemaMatch(coll, {properties: {b: {type: "boolean"}}}, {b: 1}, false);

    // type "bool" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {b: {type: "bool"}}}}).itcount());

    // bsonType "date".
    assertSchemaMatch(coll, {properties: {date: {bsonType: "date"}}}, {date: new Date()}, true);
    assertSchemaMatch(coll, {properties: {date: {bsonType: "date"}}}, {date: 1}, false);

    // type "date" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {b: {type: "date"}}}}).itcount());

    // bsonType "null".
    assertSchemaMatch(
        coll, {properties: {n: {bsonType: "null"}}, required: ["n"]}, {n: null}, true);
    assertSchemaMatch(coll, {properties: {n: {bsonType: "null"}}, required: ["n"]}, {}, false);
    assertSchemaMatch(
        coll, {properties: {n: {bsonType: "null"}}, required: ["n"]}, {u: undefined}, false);

    // type "null".
    assertSchemaMatch(coll, {properties: {n: {type: "null"}}, required: ["n"]}, {n: null}, true);
    assertSchemaMatch(coll, {properties: {n: {type: "null"}}, required: ["n"]}, {}, false);
    assertSchemaMatch(
        coll, {properties: {n: {type: "null"}}, required: ["n"]}, {u: undefined}, false);

    // bsonType "regex".
    assertSchemaMatch(coll, {properties: {r: {bsonType: "regex"}}}, {r: /^abc/}, true);
    assertSchemaMatch(coll, {properties: {r: {bsonType: "regex"}}}, {r: "^abc"}, false);

    // type "regex" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {r: {type: "regex"}}}}).itcount());

    // bsonType "javascript".
    assertSchemaMatch(coll,
                      {properties: {code: {bsonType: "javascript"}}},
                      {code: Code("function() { return true; }")},
                      true);
    assertSchemaMatch(coll, {properties: {code: {bsonType: "javascript"}}}, {code: 1}, false);

    // type "javascript" should fail.
    assert.throws(
        () => coll.find({$jsonSchema: {properties: {code: {type: "javascript"}}}}).itcount());

    // bsonType "javascriptWithScope".
    assertSchemaMatch(coll,
                      {properties: {code: {bsonType: "javascriptWithScope"}}},
                      {code: Code("function() { return true; }", {scope: true})},
                      true);
    assertSchemaMatch(
        coll, {properties: {code: {bsonType: "javascriptWithScope"}}}, {code: 1}, false);

    // type "javascriptWithScope" should fail.
    assert.throws(() =>
                      coll.find({$jsonSchema: {properties: {code: {type: "javascriptWithScope"}}}})
                          .itcount());

    // bsonType "int".
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {num: NumberInt(3)}, true);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {num: NumberLong(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {num: 3}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {num: NumberDecimal(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {num: {}}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "int"}}}, {foo: {}}, true);

    // type "int" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "int"}}}}).itcount());

    // bsonType "integer" should fail.
    assert.throws(
        () => coll.find({$jsonSchema: {properties: {num: {bsonType: "integer"}}}}).itcount());

    // type "integer" is explicitly unsupported and should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "integer"}}}}).itcount());

    // bsonType "timestamp".
    assertSchemaMatch(
        coll, {properties: {ts: {bsonType: "timestamp"}}}, {ts: Timestamp(0, 1234)}, true);
    assertSchemaMatch(coll, {properties: {ts: {bsonType: "timestamp"}}}, {ts: new Date()}, false);

    // type "timestamp" should fail.
    assert.throws(() =>
                      coll.find({$jsonSchema: {properties: {ts: {type: "timestamp"}}}}).itcount());

    // bsonType "long".
    assertSchemaMatch(coll, {properties: {num: {bsonType: "long"}}}, {num: NumberLong(3)}, true);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "long"}}}, {num: NumberInt(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "long"}}}, {num: 3}, false);
    assertSchemaMatch(
        coll, {properties: {num: {bsonType: "long"}}}, {num: NumberDecimal(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "long"}}}, {num: {}}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "long"}}}, {foo: {}}, true);

    // type "long" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "long"}}}}).itcount());

    // bsonType "decimal".
    assertSchemaMatch(
        coll, {properties: {num: {bsonType: "decimal"}}}, {num: NumberDecimal(3)}, true);
    assertSchemaMatch(
        coll, {properties: {num: {bsonType: "decimal"}}}, {num: NumberLong(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "decimal"}}}, {num: NumberInt(3)}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "decimal"}}}, {num: 3}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "decimal"}}}, {num: {}}, false);
    assertSchemaMatch(coll, {properties: {num: {bsonType: "decimal"}}}, {foo: {}}, true);

    // type "decimal" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "decimal"}}}}).itcount());

    // bsonType "minKey".
    assertSchemaMatch(coll, {properties: {k: {bsonType: "minKey"}}}, {k: MinKey()}, true);
    assertSchemaMatch(coll, {properties: {k: {bsonType: "minKey"}}}, {k: MaxKey()}, false);

    // type "minKey" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "minKey"}}}}).itcount());

    // bsonType "maxKey".
    assertSchemaMatch(coll, {properties: {k: {bsonType: "maxKey"}}}, {k: MaxKey()}, true);
    assertSchemaMatch(coll, {properties: {k: {bsonType: "maxKey"}}}, {k: MinKey()}, false);

    // type "maxKey" should fail.
    assert.throws(() => coll.find({$jsonSchema: {properties: {num: {type: "maxKey"}}}}).itcount());

    // Test that 'bsonType' keyword rejects unknown type aliases.
    assert.throws(() =>
                      coll.find({$jsonSchema: {properties: {f: {bsonType: "unknown"}}}}).itcount());

    // Test that 'type' keyword rejects unknown type aliases.
    assert.throws(() => coll.find({$jsonSchema: {properties: {f: {type: "unknown"}}}}).itcount());

    // Specifying both "type" and "bsonType" in the same schema should fail.
    assert.throws(() => coll.find({$jsonSchema: {bsonType: "string", type: "string"}}).itcount());
    assert.throws(
        () => coll.find({$jsonSchema: {properties: {a: {bsonType: "string", type: "string"}}}})
                  .itcount());

    // "type" and "bsonType" are both allowed when they are not sibling keywords in the same
    // subschema.
    assertSchemaMatch(
        coll, {type: "object", properties: {obj: {bsonType: "object"}}}, {obj: {}}, true);
    assertSchemaMatch(
        coll, {type: "object", properties: {obj: {bsonType: "object"}}}, {obj: []}, false);
    assertSchemaMatch(coll,
                      {properties: {a: {bsonType: "long"}, b: {type: "null"}}},
                      {a: NumberLong(3), b: null},
                      true);
    assertSchemaMatch(
        coll, {properties: {a: {bsonType: "long"}, b: {type: "null"}}}, {a: NumberLong(3)}, true);
    assertSchemaMatch(
        coll, {properties: {a: {bsonType: "long"}, b: {type: "null"}}}, {b: null}, true);
    assertSchemaMatch(coll,
                      {properties: {a: {bsonType: "long"}, b: {type: "null"}}},
                      {b: null},
                      {a: 3, b: null},
                      false);
    assertSchemaMatch(coll,
                      {properties: {a: {bsonType: "long"}, b: {type: "null"}}},
                      {b: null},
                      {a: NumberLong(3), b: 3},
                      false);

    // Test that the 'type' keyword rejects an array of aliases if one of those aliases is invalid.
    assert.throws(() => coll.find({$jsonSchema: {f: {type: ["number", "objectId"]}}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {f: {type: ["object", "unknown"]}}}).itcount());

    // Test that the 'bsonType' keyword rejects an array of aliases if one of those aliases is
    // invalid.
    assert.throws(() => coll.find({$jsonSchema: {f: {bsonType: ["number", "unknown"]}}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {bsonType: ["unknown"]}}).itcount());

    // Test that the 'type' keyword rejects an array which contains a numerical type alias.
    assert.throws(() => coll.find({$jsonSchema: {f: {type: ["number", 2]}}}).itcount());

    // Test that the 'bsonType' keyword rejects an array which contains a numerical type alias.
    assert.throws(() => coll.find({$jsonSchema: {f: {bsonType: ["number", 2]}}}).itcount());

    // Test that the 'type' keyword rejects an array which contains duplicate aliases.
    assert.throws(
        () => coll.find({$jsonSchema: {f: {type: ["number", "string", "number"]}}}).itcount());

    // Test that the 'bsonType' keyword rejects an array which contains duplicate aliases.
    assert.throws(
        () => coll.find({$jsonSchema: {f: {bsonType: ["number", "string", "number"]}}}).itcount());

    // Test that the 'type' keyword can accept an array of type aliases.
    assertSchemaMatch(coll, {properties: {f: {type: ["number", "string"]}}}, {f: 1}, true);
    assertSchemaMatch(coll, {properties: {f: {type: ["number", "string"]}}}, {f: "str"}, true);
    assertSchemaMatch(coll, {properties: {f: {type: ["number", "string"]}}}, {}, true);
    assertSchemaMatch(
        coll, {properties: {f: {type: ["number", "string"]}}}, {f: ["str", 1]}, false);
    assertSchemaMatch(coll, {properties: {f: {type: ["number", "string"]}}}, {f: {}}, false);

    // Test that the 'bsonType' keyword can accept an array of type aliases.
    assertSchemaMatch(coll, {properties: {f: {bsonType: ["objectId", "double"]}}}, {f: 1}, true);
    assertSchemaMatch(
        coll, {properties: {f: {bsonType: ["objectId", "double"]}}}, {f: ObjectId()}, true);
    assertSchemaMatch(coll, {properties: {f: {bsonType: ["objectId", "double"]}}}, {}, true);
    assertSchemaMatch(coll, {properties: {f: {bsonType: ["objectId", "double"]}}}, {f: [1]}, false);
    assertSchemaMatch(
        coll, {properties: {f: {bsonType: ["objectId", "double"]}}}, {f: NumberInt(1)}, false);

    // Test that the 'type' keyword with an array of types is valid at the top-level.
    assertSchemaMatch(coll, {type: ["object", "string"]}, {}, true);
    assertSchemaMatch(coll, {type: ["object", "string"]}, {foo: 1, bar: 1}, true);

    // Test that the 'bsonType' keyword with an array of types is valid at the top-level.
    assertSchemaMatch(coll, {bsonType: ["object", "double"]}, {}, true);
    assertSchemaMatch(coll, {bsonType: ["object", "double"]}, {foo: 1, bar: 1}, true);
}());
