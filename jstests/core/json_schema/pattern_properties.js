// @tags: [requires_non_retryable_commands]

/**
 * Tests for the JSON Schema 'patternProperties' keyword.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.schema_pattern_properties;

    // Test top-level patternProperties.
    assertSchemaMatch(
        coll, {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}, {}, true);
    assertSchemaMatch(
        coll, {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}, {c: 1}, true);
    assertSchemaMatch(coll,
                      {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}},
                      {ca: 1, cb: 1},
                      true);
    assertSchemaMatch(coll,
                      {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}},
                      {a: "str", ca: 1, cb: 1},
                      false);
    assertSchemaMatch(coll,
                      {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}},
                      {a: 1, b: 1, ca: 1, cb: 1},
                      false);
    assertSchemaMatch(coll,
                      {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}},
                      {a: 1, b: "str", ca: 1, cb: 1},
                      true);

    // Test patternProperties within a nested schema.
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: 1},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: {}},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: {ca: 1, cb: 1}},
        true);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: {ac: "str", ca: 1, cb: 1}},
        false);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: {ac: 1, bc: 1, ca: 1, cb: 1}},
        false);
    assertSchemaMatch(
        coll,
        {properties: {obj: {patternProperties: {"^a": {type: "number"}, "^b": {type: "string"}}}}},
        {obj: {ac: 1, bc: "str", ca: 1, cb: 1}},
        true);

    // Test that 'patternProperties' still applies, even if the field name also appears in
    // 'properties'.
    assertSchemaMatch(
        coll,
        {properties: {aa: {type: "number"}}, patternProperties: {"^a": {type: "string"}}},
        {aa: 1},
        false);
    assertSchemaMatch(coll,
                      {
                        properties: {
                            obj: {
                                properties: {aa: {type: "number"}},
                                patternProperties: {"^a": {type: "string"}}
                            }
                        }
                      },
                      {obj: {aa: 1}},
                      false);
}());
