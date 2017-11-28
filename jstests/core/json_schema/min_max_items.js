// @tags: [requires_non_retryable_commands]

/**
 * Tests the JSON Schema keywords "minItems" and "maxItems".
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.getCollection("json_schema_min_max_items");
    coll.drop();

    // Test that the JSON Schema fails to parse if "minItems" is not a valid number.
    assert.throws(() => coll.find({$jsonSchema: {minItems: "blah"}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {minItems: -1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {minItems: 12.5}}).itcount());

    // Test that "minItems" matches when the field is missing or not an array.
    assertSchemaMatch(coll, {properties: {a: {minItems: 1}}}, {}, true);
    assertSchemaMatch(coll, {properties: {a: {minItems: 1}}}, {a: "foo"}, true);

    // Test that "minItems" matches arrays with the requisite number of items.
    assertSchemaMatch(coll, {properties: {a: {minItems: 1}}}, {a: []}, false);
    assertSchemaMatch(coll, {properties: {a: {minItems: 1}}}, {a: ["x"]}, true);
    assertSchemaMatch(coll, {properties: {a: {minItems: 1}}}, {a: [0, 1]}, true);

    // Test that "minItems" has no effect when specified at the top level.
    assertSchemaMatch(coll, {minItems: 2}, {}, true);

    // Test that the JSON Schema fails to parse if "maxItems" is not a valid number.
    assert.throws(() => coll.find({$jsonSchema: {maxItems: "blah"}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {maxItems: -1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {maxItems: 12.5}}).itcount());

    // Test that "maxItems" matches when the field is missing or not an array.
    assertSchemaMatch(coll, {properties: {a: {maxItems: 1}}}, {}, true);
    assertSchemaMatch(coll, {properties: {a: {maxItems: 1}}}, {a: "foo"}, true);

    // Test that "maxItems" matches arrays with the requisite number of items.
    assertSchemaMatch(coll, {properties: {a: {maxItems: 1}}}, {a: []}, true);
    assertSchemaMatch(coll, {properties: {a: {maxItems: 1}}}, {a: ["x"]}, true);
    assertSchemaMatch(coll, {properties: {a: {maxItems: 1}}}, {a: [0, 1]}, false);

    // Test that "maxItems" has no effect when specified at the top level.
    assertSchemaMatch(coll, {maxItems: 2}, {}, true);
}());
