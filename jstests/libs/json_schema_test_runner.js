/**
 * Test runner responsible for parsing and executing a JSON-Schema-Test-Suite json file.
 */
(function() {
    "use strict";

    const coll = db.json_schema_test_corpus;
    coll.drop();

    const jsonFilename = jsTestOptions().jsonSchemaTestFile;

    if (jsonFilename === undefined) {
        throw new Error('JSON Schema tests must be run through resmoke.py');
    }

    function runSchemaTest(test, schema) {
        assert(test.hasOwnProperty("data"), "JSON Schema test requires 'data'");
        assert(test.hasOwnProperty("valid"), "JSON Schema test requires 'valid'");
        const data = test["data"];
        const valid = test["valid"];

        coll.drop();
        assert.writeOK(coll.insert({foo: data}));

        let actualCount;
        try {
            actualCount = coll.find({$jsonSchema: {properties: {foo: schema}}}).itcount();
        } catch (e) {
            throw new Error(tojson(e) + ": Failed to parse JSON Schema " + tojson(schema) +
                            " and data : " + tojson(data));
        }

        const expectedCount = valid ? 1 : 0;
        assert.eq(
            expectedCount,
            actualCount,
            "JSON Schema test failed for schema " + tojson(schema) + " and data " + tojson(data));
    }

    const testGroupList = JSON.parse(cat(jsonFilename));
    testGroupList.forEach(function(testGroup) {
        assert(testGroup.hasOwnProperty("schema"), "JSON Schema test requires a 'schema'");
        assert(testGroup.hasOwnProperty("tests"), "JSON Schema test requires a 'tests' list");
        testGroup["tests"].forEach(test => runSchemaTest(test, testGroup["schema"]));
    });
}());
