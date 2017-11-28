// @tags: [requires_non_retryable_commands]

/**
 * Tests for the JSON Schema 'additionalProperties' keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.schema_allowed_properties;

    // Tests for {additionalProperties:false} at the top level.
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: false}, {_id: 1}, true);
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: false}, {_id: 1, a: 1}, true);
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: false}, {_id: 1, b: 1}, false);
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: false},
                      {_id: 1, a: 1, b: 1},
                      false);

    // Tests for {additionalProperties:true} at the top level.
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: true}, {_id: 1}, true);
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: true}, {_id: 1, a: 1}, true);
    assertSchemaMatch(
        coll, {properties: {_id: {}, a: {}}, additionalProperties: true}, {_id: 1, b: 1}, true);
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: true},
                      {_id: 1, a: 1, b: 1},
                      true);

    // Tests for additionalProperties with a nested schema at the top level.
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: {type: "number"}},
                      {_id: 1},
                      true);
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: {type: "number"}},
                      {_id: 1, a: 1},
                      true);
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: {type: "number"}},
                      {_id: 1, b: 1},
                      true);
    assertSchemaMatch(coll,
                      {properties: {_id: {}, a: {}}, additionalProperties: {type: "number"}},
                      {_id: 1, b: "str"},
                      false);

    // Tests for additionalProperties together with patternProperties at the top level.
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1, a: 1},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1, a: 1, ba: "str"},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1, a: 1, ba: "str", other: 1},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1, a: 1, ba: "str", other: "str"},
                      false);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: {type: "number"}
                      },
                      {_id: 1, a: 1, ba: 1, other: 1},
                      false);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: false
                      },
                      {_id: 1, a: 1, ba: "str"},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {_id: {}, a: {}},
                        patternProperties: {"^b": {type: "string"}},
                        additionalProperties: false
                      },
                      {_id: 1, a: 1, ba: "str", other: 1},
                      false);

    // Tests for {additionalProperties:false} in a nested schema.
    assertSchemaMatch(
        coll, {properties: {obj: {properties: {a: {}}, additionalProperties: false}}}, {}, true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: false}}},
                      {obj: 1},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: false}}},
                      {obj: {}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: false}}},
                      {obj: {a: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: false}}},
                      {obj: {a: 1, b: 1}},
                      false);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: false}}},
                      {obj: {b: 1}},
                      false);

    // Tests for {additionalProperties:true} in a nested schema.
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: true}}},
                      {obj: {}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: true}}},
                      {obj: {a: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: true}}},
                      {obj: {a: 1, b: 1}},
                      true);
    assertSchemaMatch(coll,
                      {properties: {obj: {properties: {a: {}}, additionalProperties: true}}},
                      {obj: {b: 1}},
                      true);

    // Tests for additionalProperties whose value is a nested schema, which is itself contained
    // within a nested schema.
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: 1},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: {}},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: {a: 1}},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: {a: 1, b: 1}},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: {a: 1, b: "str"}},
        false);
    assertSchemaMatch(
        coll,
        {properties: {obj: {properties: {a: {}}, additionalProperties: {type: "number"}}}},
        {obj: {b: "str"}},
        false);

    // Tests for additionalProperties together with patternProperties, both inside a nested schema.
    assertSchemaMatch(coll,
                      {
                        properties: {
                            obj: {
                                properties: {a: {}},
                                patternProperties: {"^b": {type: "string"}},
                                additionalProperties: {type: "number"}
                            }
                        }
                      },
                      {obj: {}},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {
                            obj: {
                                properties: {a: {}},
                                patternProperties: {"^b": {type: "string"}},
                                additionalProperties: {type: "number"}
                            }
                        }
                      },
                      {obj: {a: 1, ba: "str", c: 1}},
                      true);
    assertSchemaMatch(coll,
                      {
                        properties: {
                            obj: {
                                properties: {a: {}},
                                patternProperties: {"^b": {type: "string"}},
                                additionalProperties: {type: "number"}
                            }
                        }
                      },
                      {obj: {a: 1, ba: 1, c: 1}},
                      false);
    assertSchemaMatch(coll,
                      {
                        properties: {
                            obj: {
                                properties: {a: {}},
                                patternProperties: {"^b": {type: "string"}},
                                additionalProperties: {type: "number"}
                            }
                        }
                      },
                      {obj: {a: 1, ba: 1, c: "str"}},
                      false);
}());
