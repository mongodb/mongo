// @tags: [requires_non_retryable_commands]

/**
 * Tests the JSON Schema "items" keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.getCollection("json_schema_items");
    coll.drop();

    // Test that the JSON Schema fails to parse if "items" is not an object or array.
    assert.throws(() => coll.find({$jsonSchema: {items: 1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {items: 1.0}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {items: "true"}}).itcount());

    // Test that "items" has no effect at the top level (but is still accepted).
    assertSchemaMatch(coll, {items: {type: "number"}}, {}, true);
    assertSchemaMatch(coll, {items: [{type: "number"}]}, {}, true);

    // Test that "items" matches documents where the field is missing or not an array.
    assertSchemaMatch(coll, {properties: {a: {items: {minimum: 0}}}}, {}, true);
    assertSchemaMatch(coll, {properties: {a: {items: {minimum: 0}}}}, {a: -1}, true);
    assertSchemaMatch(coll, {properties: {a: {items: [{minimum: 0}]}}}, {}, true);
    assertSchemaMatch(coll, {properties: {a: {items: [{minimum: 0}]}}}, {a: -1}, true);

    // Test that when "items" is an object, the schema applies to all elements of the array.
    let schema = {properties: {a: {items: {pattern: "a+b"}}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [7]}, true);
    assertSchemaMatch(coll, schema, {a: [null]}, true);
    assertSchemaMatch(coll, schema, {a: ["cab"]}, true);
    assertSchemaMatch(coll, schema, {a: ["cab", "caab"]}, true);
    assertSchemaMatch(coll, schema, {a: ["cab", "caab", "b"]}, false);

    // Test that when "items" is an array, each element schema only apply to elements at that
    // position.
    schema = {properties: {a: {items: [{multipleOf: 2}]}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [2]}, true);
    assertSchemaMatch(coll, schema, {a: [2, 3]}, true);
    assertSchemaMatch(coll, schema, {a: [3]}, false);

    schema = {properties: {a: {items: [{maxLength: 1}, {maxLength: 2}]}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: ["1"]}, true);
    assertSchemaMatch(coll, schema, {a: ["1"]}, true);
    assertSchemaMatch(coll, schema, {a: ["1", "12"]}, true);
    assertSchemaMatch(coll, schema, {a: ["1", "12", "123"]}, true);
    assertSchemaMatch(coll, schema, {a: ["12"]}, false);
    assertSchemaMatch(coll, schema, {a: ["1", "123"]}, false);

    // Test that "items" has no effect when it is an empty array (but is still accepted).
    schema = {properties: {a: {items: []}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "blah"}, true);
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1, "foo", {}]}, true);
}());
