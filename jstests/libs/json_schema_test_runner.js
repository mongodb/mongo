/**
 * Test runner responsible for parsing and executing a JSON-Schema-Test-Suite json file.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const coll = db.json_schema_test_corpus;
    coll.drop();

    const jsonFilename = jsTestOptions().jsonSchemaTestFile;

    if (jsonFilename === undefined) {
        throw new Error('JSON Schema tests must be run through resmoke.py');
    }

    function runSchemaTest(test, schema, banFromTopLevel) {
        assert(test.hasOwnProperty("data"), "JSON Schema test requires 'data'");
        assert(test.hasOwnProperty("valid"), "JSON Schema test requires 'valid'");
        const data = test["data"];
        const valid = test["valid"];

        try {
            assertSchemaMatch(coll,
                              {properties: {schema_test_wrapper: schema}},
                              {schema_test_wrapper: data},
                              valid);

            // Run against a top-level schema if the data is an object, since MongoDB only stores
            // records as documents.
            // (Note: JS notion of an 'object' includes arrays and null.)
            if (typeof data === "object" && !Array.isArray(data) && data !== null &&
                banFromTopLevel !== true) {
                assertSchemaMatch(coll, schema, data, valid);
            }
        } catch (e) {
            throw new Error(tojson(e) + "\n\nJSON Schema test failed for schema " + tojson(schema) +
                            " and data " + tojson(data));
        }
    }

    const testGroupList = JSON.parse(cat(jsonFilename));
    testGroupList.forEach(function(testGroup) {
        assert(testGroup.hasOwnProperty("schema"), "JSON Schema test requires a 'schema'");
        assert(testGroup.hasOwnProperty("tests"), "JSON Schema test requires a 'tests' list");
        testGroup["tests"].forEach(
            test => runSchemaTest(test, testGroup["schema"], testGroup["banFromTopLevel"]));
    });
}());
