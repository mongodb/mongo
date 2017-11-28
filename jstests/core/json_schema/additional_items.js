// @tags: [requires_non_retryable_commands]

/**
 * Tests the JSON Schema "additionalItems" keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.getCollection("json_schema_additional_items");
    coll.drop();

    // Test that the JSON Schema fails to parse if "additionalItems" is not a boolean or object.
    assert.throws(() => coll.find({$jsonSchema: {additionalItems: 1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {additionalItems: 1.0}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {additionalItems: "true"}}).itcount());

    // Test that "additionalItems" has no effect at the top level (but is still accepted).
    assertSchemaMatch(coll, {items: [{type: "number"}], additionalItems: false}, {}, true);
    assertSchemaMatch(coll, {items: [{type: "number"}], additionalItems: true}, {}, true);
    assertSchemaMatch(
        coll, {items: [{type: "number"}], additionalItems: {type: "string"}}, {}, true);

    // Test that "additionalItems" has no effect when "items" is not present.
    let schema = {properties: {a: {additionalItems: false}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "blah"}, true);
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);

    schema = {properties: {a: {additionalItems: {type: "object"}}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "blah"}, true);
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);

    // Test that "additionalItems" has no effect when "items" is a schema that applies to every
    // element in the array.
    schema = {properties: {a: {items: {}, additionalItems: false}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "blah"}, true);
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);

    schema = {properties: {a: {items: {}, additionalItems: {type: "object"}}}};
    assertSchemaMatch(coll, schema, {}, true);
    assertSchemaMatch(coll, schema, {a: "blah"}, true);
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [1, 2, 3]}, true);

    // Test that {additionalItems: false} correctly bans array indexes not covered by "items".
    schema = {
        properties: {a: {items: [{type: "number"}, {type: "string"}], additionalItems: false}}
    };
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [229]}, true);
    assertSchemaMatch(coll, schema, {a: [229, "West 43rd"]}, true);
    assertSchemaMatch(coll, schema, {a: [229, "West 43rd", "Street"]}, false);

    // Test that {additionalItems: true} has no effect.
    assertSchemaMatch(
        coll,
        {properties: {a: {items: [{type: "number"}, {type: "string"}], additionalItems: true}}},
        {a: [229, "West 43rd", "Street"]},
        true);
    assertSchemaMatch(
        coll, {properties: {a: {items: [{not: {}}], additionalItems: true}}}, {a: []}, true);

    // Test that the "additionalItems" schema only applies to array indexes not covered by "items".
    schema = {
        properties:
            {a: {items: [{type: "number"}, {type: "string"}], additionalItems: {type: "object"}}}
    };
    assertSchemaMatch(coll, schema, {a: []}, true);
    assertSchemaMatch(coll, schema, {a: [229]}, true);
    assertSchemaMatch(coll, schema, {a: [229, "West 43rd"]}, true);
    assertSchemaMatch(coll, schema, {a: [229, "West 43rd", "Street"]}, false);
    assertSchemaMatch(coll, schema, {a: [229, "West 43rd", {}]}, true);

    // Test that an empty array does not fail against "additionalItems".
    assertSchemaMatch(
        coll, {properties: {a: {items: [{not: {}}], additionalItems: false}}}, {a: []}, true);
    assertSchemaMatch(
        coll, {properties: {a: {items: [{not: {}}], additionalItems: {not: {}}}}}, {a: []}, true);
}());
