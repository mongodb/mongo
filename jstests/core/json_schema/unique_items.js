// @tags: [requires_non_retryable_commands]

/**
 * Tests the JSON Schema "uniqueItems" keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.getCollection("json_schema_unique_items");
    coll.drop();

    // Test that the JSON Schema fails to parse if "uniqueItems" is not a boolean.
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: 1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: 1.0}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: "true"}}).itcount());

    // Test that "uniqueItems" has no effect at the top level (but still succeeds).
    assertSchemaMatch(coll, {uniqueItems: true}, {}, true);
    assertSchemaMatch(coll, {uniqueItems: false}, {}, true);

    // Test that "uniqueItems" matches when the field is missing or not an array.
    let schema = {properties: {a: {uniqueItems: true}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "foo"}, true);
    assertSchemaMatch(coll, schema, {a: {foo: [1, 1], bar: [2, 2]}}, true);

    // Test that {uniqueItems: true} matches arrays whose items are all unique.
    schema = {properties: {a: {uniqueItems: true}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1]}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);
    assertSchemaMatch(coll, schema, {a: ["foo", "FOO"]}, true);
    assertSchemaMatch(coll, schema, {a: [{}, "", [], null]}, true);
    assertSchemaMatch(coll, schema, {a: [[1, 2], [2, 1]]}, true);

    // Test that {uniqueItems: true} rejects arrays with duplicates.
    schema = {properties: {a: {uniqueItems: true}}};
    assertSchemaMatch(coll, schema, {a: [1, 1]}, false);
    assertSchemaMatch(coll, schema, {a: [NumberLong(1), NumberInt(1)]}, false);
    assertSchemaMatch(coll, schema, {a: ["foo", "foo"]}, false);
    assertSchemaMatch(coll, schema, {a: [{a: 1}, {a: 1}]}, false);
    assertSchemaMatch(coll, schema, {a: [[1, 2], [1, 2]]}, false);
    assertSchemaMatch(coll, schema, {a: [null, null]}, false);
    assertSchemaMatch(coll, schema, {a: [{x: 1, y: 1}, {y: 1, x: 1}]}, false);
    assertSchemaMatch(coll, schema, {a: [{x: [1, 2], y: "a"}, {y: "a", x: [1, 2]}]}, false);

    // Test that {uniqueItems: false} has no effect.
    schema = {properties: {a: {uniqueItems: false}}};
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1]}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);
    assertSchemaMatch(coll, schema, {a: ["foo", "FOO"]}, true);
    assertSchemaMatch(coll, schema, {a: [{}, "", [], null]}, true);
    assertSchemaMatch(coll, schema, {a: [[1, 2], [2, 1]]}, true);
    assertSchemaMatch(coll, schema, {a: [1, 1]}, true);
    assertSchemaMatch(coll, schema, {a: [NumberLong(1), NumberInt(1)]}, true);
    assertSchemaMatch(coll, schema, {a: ["foo", "foo"]}, true);
    assertSchemaMatch(coll, schema, {a: [{a: 1}, {a: 1}]}, true);
    assertSchemaMatch(coll, schema, {a: [[1, 2], [1, 2]]}, true);
    assertSchemaMatch(coll, schema, {a: [null, null]}, true);
    assertSchemaMatch(coll, schema, {a: [{x: 1, y: 1}, {y: 1, x: 1}]}, true);
    assertSchemaMatch(coll, schema, {a: [{x: [1, 2], y: "a"}, {y: "a", x: [1, 2]}]}, true);
}());
