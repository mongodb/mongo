// @tags: [requires_non_retryable_commands]

/**
 * Tests for handling of the JSON Schema 'required' keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.jstests_schema_required;

    assertSchemaMatch(coll, {required: ["a"]}, {a: 1}, true);
    assertSchemaMatch(coll, {required: ["a"]}, {}, false);
    assertSchemaMatch(coll, {required: ["a"]}, {b: 1}, false);
    assertSchemaMatch(coll, {required: ["a"]}, {b: {a: 1}}, false);

    assertSchemaMatch(coll, {required: ["a", "b"]}, {a: 1, b: 1, c: 1}, true);
    assertSchemaMatch(coll, {required: ["a", "b"]}, {a: 1, c: 1}, false);
    assertSchemaMatch(coll, {required: ["a", "b"]}, {b: 1, c: 1}, false);

    assertSchemaMatch(coll, {properties: {a: {required: ["b"]}}}, {}, true);
    assertSchemaMatch(coll, {properties: {a: {required: ["b"]}}}, {a: 1}, true);
    assertSchemaMatch(coll, {properties: {a: {required: ["b"]}}}, {a: {b: 1}}, true);
    assertSchemaMatch(coll, {properties: {a: {required: ["b"]}}}, {a: {c: 1}}, false);
    assertSchemaMatch(coll, {properties: {a: {required: ["b"]}}}, {a: {}}, false);
}());
