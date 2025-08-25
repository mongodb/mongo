/**
 * Tests behavior of conversions to string to array and object.
 *
 * @tags: [
 *   featureFlagMqlJsEngineGap,
 *   requires_fcv_83,
 * ]
 */

import {runConvertTests} from "jstests/libs/query/convert_shared.js";

const coll = db.expression_convert_bindata;
coll.drop();

const requiresFCV83 = true;

function makeNestedArrayString(n) {
    return ["[".repeat(n), '"1"', "]".repeat(n)].join("");
}

function makeLongArrayString(n) {
    return ["[", '"1",'.repeat(n - 1), '"1"]'].join("");
}

function makeValidConversion(_id, input) {
    const expected = JSON.parse(input);
    const target = Array.isArray(expected) ? "array" : "object";
    return {_id, input, target, expected};
}

const conversionTestDocs = [
    // Simple cases.
    makeValidConversion(0, "[]"),
    makeValidConversion(1, '["bar"]'),
    makeValidConversion(2, '{"foo": "bar"}'),
    makeValidConversion(3, "[null]"),
    makeValidConversion(4, "[false]"),
    makeValidConversion(5, "[true]"),
    makeValidConversion(6, "[-999]"),
    makeValidConversion(7, '{"embedded-null": "her\\u0000e"}'),
    makeValidConversion(8, '["nul\\u0000l"]'),
    makeValidConversion(9, '[[[["nested", {"object":"here"}]]]]'),
    makeValidConversion(10, '{"nested": [[["array"]]]}'),
    // Very deep and wide (but not too deep or large) inputs.
    makeValidConversion(11, `{"nested145": ${makeNestedArrayString(145)}, "another": ${makeNestedArrayString(145)}}`),
    makeValidConversion(12, `{"long100K": ${makeLongArrayString(100_000)}}`),
    // Numeric conversions.
    makeValidConversion(13, '{"number": 1.2e+3}'),
    makeValidConversion(14, '{"number": 18446744073709551615}'),
    makeValidConversion(15, '{"number": -18446744073709551615}'),
    // Non-ascii characters
    makeValidConversion(16, '{"車B1234 こんにちは": "車B1234 こんにちは"}'),
    // To protect against injections, ensure we don't attempt to parse/execute the resulting object
    // as MQL.
    makeValidConversion(17, '{"$toLong": ";;]"}'),
    makeValidConversion(18, '{"$toString": "$missing"}'),
    makeValidConversion(19, '["$missing", "$input"]'),
    makeValidConversion(20, '{"$literal": 123}'),
    makeValidConversion(21, '{"$toString": {}}'),
    makeValidConversion(22, '{"$toString": []}'),
];

const illegalConversionTestDocs = [
    // We hit the BSON depth limit.
    {_id: 0, input: `{"nested200": ${makeNestedArrayString(200)}}`, target: "object"},
    // We hit the BSON size limit (because BSON arrays are stored like {"0": val, "1": val}).
    {_id: 1, input: `[${makeLongArrayString(1_200_000)}]`, target: "array"},
    {_id: 2, input: `{"large": ${makeLongArrayString(1_200_000)}}`, target: "object"},
    // Valid input but mismatched target type.
    {_id: 3, input: "[{}]", target: "object"},
    {_id: 4, input: '{"f": []}', target: "array"},
    // Technically valid JSON but not a top-level object or array.
    {_id: 5, input: '"str"', target: "object"},
    {_id: 6, input: "123", target: "object"},
    {_id: 7, input: "true", target: "array"},
    {_id: 8, input: "null", target: "object"},
    // Embedded null in key string.
    {_id: 9, input: '{"f\\u0000oo": 1}', target: "object"},
    // Unescaped embedded null anywhere.
    {_id: 10, input: '{"foo": "b\\0ar"}', target: "object"},
    {_id: 11, input: '{"f\\0oo": "bar"}', target: "object"},
    {_id: 12, input: '{\\0"foo": "bar"}', target: "object"},
    // Unsupported JSON-type syntax like trailing commas, jsonlines etc.
    {_id: 13, input: '{"foo": 1}\n{"foo": 2}', target: "object"},
    {_id: 14, input: '{"foo": 1}\n{"foo": 2}', target: "array"},
    {_id: 15, input: '{"foo": 1,}', target: "object"},
    {_id: 16, input: '["foo",1,]', target: "array"},
    {_id: 17, input: '["foo",,]', target: "array"},
    // MQL syntax, i.e. not valid JSON.
    {_id: 18, input: "{$literal: 123}", target: "object"},
    {_id: 19, input: "[{$literal: 123}]", target: "array"},
    {_id: 20, input: "{$toString: {}}", target: "object"},
];

// One test document for each "nullish" value.
const nullTestDocs = [{_id: 0, input: null}, {_id: 1, input: undefined}, {_id: 2 /* input is missing */}];

runConvertTests({coll, requiresFCV83, conversionTestDocs, illegalConversionTestDocs, nullTestDocs});
