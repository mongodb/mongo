/**
 * Test that setting the query knob 'internalQueryIgnoreUnknownJSONSchemaKeywords' correctly
 * ignores unknown keywords within $jsonSchema.
 */
(function() {
    "use strict";

    load("jstests/libs/assert_schema_match.js");

    const options = {setParameter: "internalQueryIgnoreUnknownJSONSchemaKeywords=1"};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

    const testDB = conn.getDB("test");
    const coll = testDB.getCollection("jstests_json_schema_ignore_unsupported");

    assertSchemaMatch(coll, {my_keyword: "ignored", minProperties: 2}, {_id: 0}, false);
    assertSchemaMatch(coll, {my_keyword: "ignored", minProperties: 2}, {_id: 0, a: 1}, true);
    assertSchemaMatch(
        coll, {properties: {a: {my_keyword: "ignored", minProperties: 1}}}, {a: {b: 1}}, true);

    // Test that the same query knob does not change the behavior for unsupported keywords.
    {
        let res =
            coll.runCommand({find: coll.getName(), query: {$jsonSchema: {default: {_id: 0}}}});
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res = coll.runCommand({
            find: coll.getName(),
            query: {$jsonSchema: {definitions: {numberField: {type: "number"}}}}
        });
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res = coll.runCommand({find: coll.getName(), query: {$jsonSchema: {format: "email"}}});
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res =
            coll.runCommand({find: coll.getName(), query: {$jsonSchema: {id: "someschema.json"}}});
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res = coll.runCommand({
            find: coll.getName(),
            query: {$jsonSchema: {properties: {a: {$ref: "#/definitions/positiveInt"}}}}
        });
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res = coll.runCommand(
            {find: coll.getName(), query: {$jsonSchema: {$schema: "hyper-schema"}}});
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

        res = coll.runCommand({
            find: coll.getName(),
            query: {$jsonSchema: {$schema: "http://json-schema.org/draft-04/schema#"}}
        });
        assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
    }

    MongoRunner.stopMongod(conn);
}());
