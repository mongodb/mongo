/**
 * Tests for the JSON Schema logical keywords, including:
 *
 *  - allOf
 *  - anyOf
 *  - oneOf
 *  - not
 *  - enum
 */
(function() {
    "use strict";

    const coll = db.jstests_json_schema_logical;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, foo: 3}));
    assert.writeOK(coll.insert({_id: 1, foo: -3}));
    assert.writeOK(coll.insert({_id: 2, foo: {}}));
    assert.writeOK(coll.insert({_id: 3, foo: "string"}));
    assert.writeOK(coll.insert({_id: 4, foo: ["str", 5]}));
    assert.writeOK(coll.insert({_id: 5}));

    // Test that $jsonSchema fails to parse if the values for the allOf, anyOf, and oneOf
    // keywords are not arrays of valid schema.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {allOf: [{invalid: "0"}]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {anyOf: [{invalid: "0"}]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {oneOf: [{invalid: "0"}]}}}}).itcount();
    });

    // Test that $jsonSchema fails to parse if the value for the 'not' keyword is not a
    // valid schema object.
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: {maximum: "0"}}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: [0]}}}}).itcount();
    });
    assert.throws(function() {
        coll.find({$jsonSchema: {properties: {foo: {not: [{}]}}}}).itcount();
    });

    function runFindSortedResults(schemaFilter) {
        return coll.find({$jsonSchema: schemaFilter}, {_id: 1}).sort({_id: 1}).toArray();
    }

    // Test that the 'allOf' keyword correctly returns documents that match every schema in
    // the array.
    assert.eq([{_id: 0}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({properties: {foo: {allOf: [{minimum: 0}]}}}));
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({properties: {foo: {allOf: [{}]}}}));
    assert.eq([{_id: 0}, {_id: 5}],
              runFindSortedResults({properties: {foo: {allOf: [{type: 'number'}, {minimum: 0}]}}}));

    // Test that a top-level 'allOf' keyword matches the correct documents.
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({allOf: [{}]}));
    assert.eq([{_id: 3}, {_id: 5}],
              runFindSortedResults({allOf: [{properties: {foo: {type: 'string'}}}]}));

    // Test that 'allOf' in conjunction with another keyword matches the correct documents.
    assert.eq([{_id: 0}, {_id: 5}],
              runFindSortedResults({properties: {foo: {type: "number", allOf: [{minimum: 0}]}}}));

    // Test that the 'anyOf' keyword correctly returns documents that match at least one schema
    // in the array.
    assert.eq([{_id: 0}, {_id: 3}, {_id: 5}],
              runFindSortedResults(
                  {properties: {foo: {anyOf: [{type: 'string'}, {type: 'number', minimum: 0}]}}}));
    assert.eq(
        [{_id: 2}, {_id: 3}, {_id: 5}],
        runFindSortedResults({properties: {foo: {anyOf: [{type: 'string'}, {type: 'object'}]}}}));
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({properties: {foo: {anyOf: [{}]}}}));

    // Test that a top-level 'anyOf' keyword matches the correct documents.
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({anyOf: [{}]}));
    assert.eq([{_id: 3}, {_id: 5}],
              runFindSortedResults({anyOf: [{properties: {foo: {type: 'string'}}}]}));

    // Test that 'anyOf' in conjunction with another keyword matches the correct documents.
    assert.eq([{_id: 5}],
              runFindSortedResults({properties: {foo: {type: "number", anyOf: [{minimum: 4}]}}}));

    // Test that the 'oneOf' keyword correctly returns documents that match exactly one schema
    // in the array.
    assert.eq([{_id: 1}, {_id: 5}],
              runFindSortedResults({properties: {foo: {oneOf: [{minimum: 0}, {maximum: 3}]}}}));
    assert.eq(
        [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}, {_id: 5}],
        runFindSortedResults({properties: {foo: {oneOf: [{type: 'string'}, {pattern: "ing"}]}}}));
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({properties: {foo: {oneOf: [{}]}}}));

    // Test that a top-level 'oneOf' keyword matches the correct documents.
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({oneOf: [{}]}));
    assert.eq([{_id: 3}, {_id: 5}],
              runFindSortedResults({oneOf: [{properties: {foo: {type: 'string'}}}]}));
    assert.eq([], runFindSortedResults({oneOf: [{}, {}]}));

    // Test that 'oneOf' in conjunction with another keyword matches the correct documents.
    assert.eq([{_id: 5}],
              runFindSortedResults({properties: {foo: {type: "number", oneOf: [{minimum: 4}]}}}));

    // Test that the 'not' keyword correctly returns documents that do not match any schema
    // in the array.
    assert.eq([{_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}],
              runFindSortedResults({properties: {foo: {not: {type: 'number'}}}}));
    assert.eq([{_id: 5}], runFindSortedResults({properties: {foo: {not: {}}}}));

    // Test that a top-level 'not' keyword matches the correct documents.
    assert.eq([], runFindSortedResults({not: {}}));
    assert.eq([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}],
              runFindSortedResults({not: {properties: {foo: {type: 'string'}}}}));

    // Test that 'not' in conjunction with another keyword matches the correct documents.
    assert.eq([{_id: 3}, {_id: 5}],
              runFindSortedResults({properties: {foo: {type: "string", not: {maxLength: 4}}}}));

}());
