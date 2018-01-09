// @tags: [requires_non_retryable_commands]

/**
 * Tests for the JSON Schema 'minProperties' and 'maxProperties' keywords.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.jstests_schema_min_max_properties;

    // Test that {minProperties: 0} matches any object.
    assertSchemaMatch(coll, {minProperties: 0}, {}, true);
    assertSchemaMatch(coll, {minProperties: 0}, {a: 1}, true);
    assertSchemaMatch(coll, {minProperties: 0}, {a: 1, b: 2}, true);

    // Test that {maxProperties: 0} matches nothing, since objects always must have the "_id" field
    // when inserted into a collection.
    assertSchemaMatch(coll, {maxProperties: 0}, {}, false);
    assertSchemaMatch(coll, {maxProperties: 0}, {a: 1}, false);
    assertSchemaMatch(coll, {maxProperties: 0}, {a: 1, b: 2}, false);

    // Test top-level minProperties greater than 0.
    assertSchemaMatch(coll, {minProperties: 2}, {_id: 0}, false);
    assertSchemaMatch(coll, {minProperties: 2}, {_id: 0, a: 1}, true);
    assertSchemaMatch(coll, {minProperties: 2}, {_id: 0, a: 1, b: 2}, true);

    // Test top-level maxProperties greater than 0.
    assertSchemaMatch(coll, {maxProperties: 2}, {_id: 0}, true);
    assertSchemaMatch(coll, {maxProperties: 2}, {_id: 0, a: 1}, true);
    assertSchemaMatch(coll, {maxProperties: 2}, {_id: 0, a: 1, b: 2}, false);

    // Test nested maxProperties greater than 0.
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 1}}}, {a: 1}, true);
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 1}}}, {a: {}}, true);
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 1}}}, {a: {b: 1}}, true);
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 1}}}, {a: {b: 1, c: 1}}, false);

    // Test nested maxProperties of 0.
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 0}}}, {a: {}}, true);
    assertSchemaMatch(coll, {properties: {a: {maxProperties: 0}}}, {a: {b: 1}}, false);

    // Test nested minProperties greater than 0.
    assertSchemaMatch(coll, {properties: {a: {minProperties: 1}}}, {a: 1}, true);
    assertSchemaMatch(coll, {properties: {a: {minProperties: 1}}}, {a: {}}, false);
    assertSchemaMatch(coll, {properties: {a: {minProperties: 1}}}, {a: {b: 1}}, true);
    assertSchemaMatch(coll, {properties: {a: {minProperties: 1}}}, {a: {b: 1, c: 1}}, true);
}());
