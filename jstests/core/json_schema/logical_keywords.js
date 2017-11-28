// @tags: [requires_non_retryable_commands]

/**
 * Tests for the JSON Schema logical keywords, including:
 *
 *  - allOf
 *  - anyOf
 *  - oneOf
 *  - not
 *  - enum
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.jstests_json_schema_logical;

    // Test that $jsonSchema fails to parse if the values for the allOf, anyOf, and oneOf
    // keywords are not arrays of valid schema.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: [{invalid: "0"}]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: [{invalid: "0"}]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: [{invalid: "0"}]}}}}).itcount();
    });

    // Test that $jsonSchema fails to parse if the value for the 'not' keyword is not a
    // valid schema object.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: [{}]}}}}).itcount();
    });

    // Test that the 'allOf' keyword correctly returns documents that match every schema in
    // the array.
    let schema = {properties: {foo: {allOf: [{minimum: 1}]}}};
    assertSchemaMatch(coll, schema, {foo: 1}, true);
    assertSchemaMatch(coll, schema, {foo: 0}, false);
    assertSchemaMatch(coll, schema, {foo: "string"}, true);

    schema = {properties: {foo: {allOf: [{}]}}};
    assertSchemaMatch(coll, schema, {foo: {}}, true);
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: 0}, true);

    schema = {properties: {foo: {allOf: [{type: 'number'}, {minimum: 0}]}}};
    assertSchemaMatch(coll, schema, {foo: 0}, true);
    assertSchemaMatch(coll, schema, {foo: "string"}, false);
    assertSchemaMatch(coll, schema, {foo: [0]}, false);

    // Test that a top-level 'allOf' keyword matches the correct documents.
    assertSchemaMatch(coll, {allOf: [{}]}, {}, true);
    assertSchemaMatch(coll, {allOf: [{}]}, {foo: 0}, true);
    assertSchemaMatch(coll, {allOf: [{type: 'string'}]}, {}, false);
    assertSchemaMatch(coll, {allOf: [{properties: {foo: {type: 'string'}}}]}, {foo: "str"}, true);
    assertSchemaMatch(coll, {allOf: [{properties: {foo: {type: 'string'}}}]}, {foo: 1}, false);

    // Test that 'allOf' in conjunction with another keyword matches the correct documents.
    assertSchemaMatch(
        coll, {properties: {foo: {type: "number", allOf: [{minimum: 1}]}}}, {foo: 1}, true);
    assertSchemaMatch(
        coll, {properties: {foo: {type: "number", allOf: [{minimum: 1}]}}}, {foo: "str"}, false);

    // Test that the 'anyOf' keyword correctly returns documents that match at least one schema
    // in the array.
    schema = {properties: {foo: {anyOf: [{type: 'string'}, {type: 'number', minimum: 1}]}}};
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, true);
    assertSchemaMatch(coll, schema, {foo: 0}, false);

    schema = {properties: {foo: {anyOf: [{type: 'string'}, {type: 'object'}]}}};
    assertSchemaMatch(coll, schema, {foo: {}}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: [{}]}, false);

    schema = {properties: {foo: {anyOf: [{}]}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: {}}, true);
    assertSchemaMatch(coll, schema, {foo: 0}, true);

    // Test that a top-level 'anyOf' keyword matches the correct documents.
    schema = {anyOf: [{}]};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, true);

    schema = {anyOf: [{properties: {foo: {type: 'string'}}}]};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, false);

    // Test that 'anyOf' in conjunction with another keyword matches the correct documents.
    schema = {properties: {foo: {type: "number", anyOf: [{minimum: 1}]}}};
    assertSchemaMatch(coll, schema, {foo: 1}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, false);

    // Test that the 'oneOf' keyword correctly returns documents that match exactly one schema
    // in the array.
    schema = {properties: {foo: {oneOf: [{minimum: 0}, {maximum: 3}]}}};
    assertSchemaMatch(coll, schema, {foo: 4}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, false);
    assertSchemaMatch(coll, schema, {foo: "str"}, false);

    schema = {properties: {foo: {oneOf: [{type: 'string'}, {pattern: "ing"}]}}};
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: "string"}, false);

    schema = {properties: {foo: {oneOf: [{}]}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, true);

    // Test that a top-level 'oneOf' keyword matches the correct documents.
    schema = {oneOf: [{}]};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, true);

    schema = {oneOf: [{properties: {foo: {type: 'string'}}}]};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, false);

    assertSchemaMatch(coll, {oneOf: [{}, {}]}, {}, false);

    // Test that 'oneOf' in conjunction with another keyword matches the correct documents.
    schema = {properties: {foo: {type: "number", oneOf: [{minimum: 4}]}}};
    assertSchemaMatch(coll, schema, {foo: 4}, true);
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, false);

    // Test that the 'not' keyword correctly returns documents that do not match any schema
    // in the array.
    schema = {properties: {foo: {not: {type: 'number'}}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, false);

    schema = {properties: {foo: {not: {}}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: 1}, false);

    // Test that a top-level 'not' keyword matches the correct documents.
    assertSchemaMatch(coll, {not: {}}, {}, false);

    schema = {not: {properties: {foo: {type: 'string'}}}};
    assertSchemaMatch(coll, schema, {foo: 1}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, false);
    assertSchemaMatch(coll, schema, {}, false);

    // Test that 'not' in conjunction with another keyword matches the correct documents.
    schema = {properties: {foo: {type: "string", not: {maxLength: 4}}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {foo: "string"}, true);
    assertSchemaMatch(coll, schema, {foo: "str"}, false);
    assertSchemaMatch(coll, schema, {foo: 1}, false);

    // Test that the 'enum' keyword correctly matches scalar values.
    schema = {properties: {a: {enum: ["str", 5]}}};
    assertSchemaMatch(coll, schema, {a: "str"}, true);
    assertSchemaMatch(coll, schema, {a: 5}, true);
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: ["str"]}, false);

    // Test that the 'enum' keyword with a null value correctly matches literal null elements, but
    // not 'missing' or 'undefined.
    schema = {properties: {a: {enum: [null]}}};
    assertSchemaMatch(coll, schema, {a: null}, true);
    assertSchemaMatch(coll, schema, {a: undefined}, false);
    assertSchemaMatch(coll, schema, {a: 1}, false);
    assertSchemaMatch(coll, {properties: {a: {enum: [null]}}, required: ['a']}, {}, false);

    // Test that the 'enum' keyword correctly matches array values.
    schema = {properties: {a: {enum: [[1, 2, "3"]]}}};
    assertSchemaMatch(coll, schema, {a: [1, 2, "3"]}, true);
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: [2, "3", 1]}, false);

    schema = {properties: {a: {enum: [[]]}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: [1]}, false);

    // Test that the 'enum' keyword does not traverse arrays when matching.
    schema = {properties: {a: {enum: ["str", 1]}}};
    assertSchemaMatch(coll, schema, {a: ["str"]}, false);
    assertSchemaMatch(coll, schema, {a: [1]}, false);

    // Test that the 'enum' keyword matches objects regardless of the field ordering.
    schema = {properties: {a: {enum: [{name: "tiny", size: "large"}]}}};
    assertSchemaMatch(coll, schema, {a: {name: "tiny", size: "large"}}, true);
    assertSchemaMatch(coll, schema, {a: {size: "large", name: "tiny"}}, true);

    // Test that the 'enum' keyword does not match documents with additional fields.
    assertSchemaMatch(coll,
                      {properties: {a: {enum: [{name: "tiny"}]}}},
                      {a: {size: "large", name: "tiny"}},
                      false);

    // Test that a top-level 'enum' matches the correct documents.
    assertSchemaMatch(coll, {enum: [{_id: 0}]}, {_id: 0}, true);
    assertSchemaMatch(coll, {enum: [{_id: 0, a: "str"}]}, {_id: 0, a: "str"}, true);
    assertSchemaMatch(coll, {enum: [{}]}, {}, false);
    assertSchemaMatch(coll, {enum: [null]}, {}, false);
    assertSchemaMatch(coll, {enum: [{_id: 0, a: "str"}]}, {_id: 0, a: "str", b: 1}, false);
    assertSchemaMatch(coll, {enum: [1, 2]}, {}, false);
}());
