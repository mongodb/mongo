/**
 * Tests for handling of the JSON Schema 'required' keyword.
 */
(function() {
    "use strict";

    const coll = db.jstests_schema_required;
    coll.drop();

    /**
     * If 'valid' is true, asserts that 'doc' matches 'schema', by wrapping 'schema' in a
     * $jsonSchema predicate. If valid is false, asserts that 'doc' does not match 'schema'.
     */
    function assertSchemaMatch(schema, doc, valid) {
        coll.drop();
        assert.writeOK(coll.insert(doc));
        const count = coll.find({$jsonSchema: schema}).itcount();
        assert.eq(count, valid ? 1 : 0);
    }

    assertSchemaMatch({required: ["a"]}, {a: 1}, true);
    assertSchemaMatch({required: ["a"]}, {}, false);
    assertSchemaMatch({required: ["a"]}, {b: 1}, false);
    assertSchemaMatch({required: ["a"]}, {b: {a: 1}}, false);

    assertSchemaMatch({required: ["a", "b"]}, {a: 1, b: 1, c: 1}, true);
    assertSchemaMatch({required: ["a", "b"]}, {a: 1, c: 1}, false);
    assertSchemaMatch({required: ["a", "b"]}, {b: 1, c: 1}, false);

    assertSchemaMatch({properties: {a: {required: ["b"]}}}, {}, true);
    assertSchemaMatch({properties: {a: {required: ["b"]}}}, {a: 1}, true);
    assertSchemaMatch({properties: {a: {required: ["b"]}}}, {a: {b: 1}}, true);
    assertSchemaMatch({properties: {a: {required: ["b"]}}}, {a: {c: 1}}, false);
    assertSchemaMatch({properties: {a: {required: ["b"]}}}, {a: {}}, false);
}());
