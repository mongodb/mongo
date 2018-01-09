// @tags: [requires_non_retryable_commands]

/**
 * Tests for the JSON Schema 'dependencies' keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.jstests_schema_dependencies;

    // Top-level schema dependency.
    assertSchemaMatch(coll, {dependencies: {foo: {required: ["bar"]}}}, {}, true);
    assertSchemaMatch(coll, {dependencies: {foo: {required: ["bar"]}}}, {foo: 1, bar: 1}, true);
    assertSchemaMatch(coll, {dependencies: {foo: {required: ["bar"]}}}, {bar: 1}, true);
    assertSchemaMatch(coll, {dependencies: {foo: {required: ["bar"]}}}, {foo: 1}, false);

    assertSchemaMatch(
        coll,
        {dependencies: {foo: {required: ["bar"], properties: {baz: {type: "string"}}}}},
        {},
        true);
    assertSchemaMatch(
        coll,
        {dependencies: {foo: {required: ["bar"], properties: {baz: {type: "string"}}}}},
        {bar: 1},
        true);
    assertSchemaMatch(
        coll,
        {dependencies: {foo: {required: ["bar"], properties: {baz: {type: "string"}}}}},
        {foo: 1, bar: 1},
        true);
    assertSchemaMatch(
        coll,
        {dependencies: {foo: {required: ["bar"], properties: {baz: {type: "string"}}}}},
        {foo: 1, bar: 1, baz: 1},
        false);
    assertSchemaMatch(
        coll,
        {dependencies: {foo: {required: ["bar"], properties: {baz: {type: "string"}}}}},
        {foo: 1, bar: 1, baz: "str"},
        true);

    // Top-level property dependency.
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {}, true);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {bar: 1}, true);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {baz: 1}, true);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {bar: 1, baz: 1}, true);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {foo: 1}, false);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {foo: 1, bar: 1}, false);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {foo: 1, baz: 1}, false);
    assertSchemaMatch(coll, {dependencies: {foo: ["bar", "baz"]}}, {foo: 1, bar: 1, baz: 1}, true);

    // Nested schema dependency.
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}}, {}, true);
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}}, {obj: 1}, true);
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}}, {obj: {}}, true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}},
                      {obj: {bar: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}},
                      {obj: {foo: 1}},
                      false);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {foo: {required: ["bar"]}}}}},
                      {obj: {foo: 1, bar: 1}},
                      true);

    // Nested property dependency.
    assertSchemaMatch(coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {}, true);
    assertSchemaMatch(coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {obj: 1}, true);
    assertSchemaMatch(coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {obj: {}}, true);
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {obj: {bar: 1}}, true);
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {obj: {foo: 1}}, false);
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {foo: ["bar"]}}}}, {obj: {foo: 1, bar: 1}}, true);

    // Nested property dependency and nested schema dependency.
    assertSchemaMatch(
        coll, {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}}, {}, true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: 1},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {b: 1, d: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {a: 1, b: 1, c: 1}},
                      false);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {a: 1, c: 0, d: 1}},
                      false);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {b: 1, c: 1, d: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {a: 1, b: 1, d: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {dependencies: {a: ["b"], c: {required: ["d"]}}}}},
                      {obj: {a: 1, b: 1, c: 1, d: 1}},
                      true);

    // Empty dependencies matches everything.
    assertSchemaMatch(coll, {dependencies: {}}, {}, true);
    assertSchemaMatch(coll, {properties: {obj: {dependencies: {}}}}, {obj: {}}, true);
}());
