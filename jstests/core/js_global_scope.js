// Tests that the properties available in the global scope during js execution are a limited, known
// set. This should help prevent accidental additions or leaks of functions.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
// ]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For 'resultsEq'.

// Note: It's important to use our own database here to avoid sharing a javascript execution context
// (Scope) with other tests which could pollute the global scope. This context is cached and shared
// per database in a pool for every operation using JS in the same database.
const testDB = db.getSiblingDB("js_global_scope");
const coll = testDB.test;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1}));

const expectedGlobalVars = [
    "AggregateError",
    "Array",
    "ArrayBuffer",
    "BigInt",
    "BigInt64Array",
    "BigUint64Array",
    "BinData",
    "Boolean",
    "Code",
    "DBPointer",
    "DBRef",
    "DataView",
    "Date",
    "Error",
    "EvalError",
    "Float32Array",
    "Float64Array",
    "Function",
    "HexData",
    "ISODate",
    "Infinity",
    "Int16Array",
    "Int32Array",
    "Int8Array",
    "InternalError",
    "JSON",
    "MD5",
    "Map",
    "Math",
    "MaxKey",
    "MinKey",
    "MongoURI",
    "NaN",
    "Number",
    "NumberDecimal",
    "NumberInt",
    "NumberLong",
    "Object",
    "ObjectId",
    "Promise",
    "Proxy",
    "RangeError",
    "ReferenceError",
    "RegExp",
    "Set",
    "String",
    "Symbol",
    "SyntaxError",
    "Timestamp",
    "TypeError",
    "URIError",
    "UUID",
    "Uint16Array",
    "Uint32Array",
    "Uint8Array",
    "Uint8ClampedArray",
    "WeakMap",
    "WeakSet",
    "__lastres__",
    "_convertExceptionToReturnStatus",
    "assert",
    "bsonBinaryEqual",
    "bsonObjToArray",
    "bsonUnorderedFieldsCompare",
    "bsonWoCompare",
    "buildInfo",
    "decodeURI",
    "decodeURIComponent",
    "doassert",
    "encodeURI",
    "encodeURIComponent",
    "escape",
    "eval",
    "gc",
    "getJSHeapLimitMB",
    "globalThis",
    "hex_md5",
    "isFinite",
    "isNaN",
    "isNumber",
    "isObject",
    "isString",
    "parseFloat",
    "parseInt",
    "print",
    "printjson",
    "printjsononeline",
    "sleep",
    "sortDoc",
    "tojson",
    "tojsonObject",
    "tojsononeline",
    "tostrictjson",
    "undefined",
    "unescape",
    "version",
];

const targetArch = buildInfo().buildEnvironment.target_arch;
if (targetArch !== 'aarch64' && targetArch !== 'ppc64le' && targetArch !== 's390x') {
    // WebAssembly is not supported on ARM and PPC platforms. It is not compiled in to the JS
    // environment, so isn't expected in this output.
    expectedGlobalVars.push("WebAssembly");
}
// Note: it is important that this is sorted to compare to sorted variable names below.
expectedGlobalVars.sort();

function getGlobalProps() {
    const global = function() {
        return this;
    }();
    return Object.getOwnPropertyNames(global);
}

const props =
    coll.aggregate([
            {$replaceWith: {varName: {$function: {lang: "js", args: [], body: getGlobalProps}}}},
            {$unwind: "$varName"},
            {$sort: {varName: 1}},
        ])
        .toArray();

// Because both are sorted, we can pinpoint any that are missing by going in order.
for (let i = 0; i < Math.min(expectedGlobalVars.length, props.length); ++i) {
    const foundProp = props[i].varName;
    const expectedProp = expectedGlobalVars[i];
    assert.eq(foundProp, expectedProp, () => {
        if (foundProp < expectedProp) {
            return `Found an unexpected extra global property during JS execution: "${
                foundProp}".\n Expected only ${tojson(expectedGlobalVars)}.\n Found ${
                tojson(props)}.`;
        } else {
            return `Did not find an expected global property during JS execution: "${
                expectedProp}".\n Expected only ${tojson(expectedGlobalVars)}.\n Found ${
                tojson(props)}.`;
        }
    });
}
assert.lte(expectedGlobalVars.length,
           props.length,
           () => `Did not find expected global properties during JS execution: ${
               tojson(expectedGlobalVars.slice(props.length))}.\n Full list expected: ${
               tojson(expectedGlobalVars)}.\n Full list found: ${tojson(props)}.`);
assert.lte(props.length,
           expectedGlobalVars.length,
           () => `Found extra global properties during JS execution: ${
               tojson(props.slice(expectedGlobalVars.length))}`);

// Now test the same properties appear in a $where. We have two additional expected properties which
// are defined by $where itself before executing the filter function.
const expectedVarsInWhere = expectedGlobalVars.concat(["obj", "fullObject"]);
assert.eq(
    coll.find().itcount(),
    coll.find({
            $where: "const global = function() { return this; }();\n" +
                "printjsononeline(Object.getOwnPropertyNames(global));\n" +
                `printjsononeline(${tojsononeline(expectedVarsInWhere)});\n` +
                "const found = new Set(Object.getOwnPropertyNames(global));\n" +
                `const expected = new Set(${tojsononeline(expectedVarsInWhere)});\n` +
                "for (let foundItem of found) {\n" +
                // __returnValue is a special case that we allow. This is populated _after_ we call
                // the function in order to communicate the return value. Thus, we don't expect it
                // on the first invocation, but it could happen if this test is run multiple times
                // in a row.
                "    if (!expected.has(foundItem) && foundItem != '__returnValue') {\n" +
                "        print('found extra item: ' + foundItem); return false;\n" +
                "    }\n" +
                "}\n" +
                "for (let expectedItem of expected) {\n" +
                "    if (!found.has(expectedItem)) {\n" +
                "        print('did not find expected item: ' + expectedItem); return false;\n" +
                "    }\n" +
                "}\n" +
                "return true;"
        })
        .itcount());
}());
