/**
 * Tests JSON Schema keywords related to arrays:
 *   - minItems
 *   - maxItems
 */
(function() {
    "use strict";

    const coll = db.getCollection("json_schema_arrays");
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: 1}));
    assert.writeOK(coll.insert({_id: 1, a: []}));
    assert.writeOK(coll.insert({_id: 2, a: [2, "str"]}));
    assert.writeOK(coll.insert({_id: 3, a: [3, 3, "foo"]}));
    assert.writeOK(coll.insert({_id: 4}));

    function assertFindResultsSortedEq(query, expected) {
        assert.eq(coll.find(query, {_id: 1}).sort({_id: 1}).toArray(),
                  expected,
                  "JSON Schema keyword did not match the expected documents");
    }

    // Test that the JSON Schema fails to parse if "minItems" is not a valid number.
    assert.throws(() => coll.find({$jsonSchema: {minItems: "blah"}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {minItems: -1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {minItems: 12.5}}).itcount());

    // Test that "minItems" matches non-arrays, or arrays with at least the given number of items.
    assertFindResultsSortedEq({$jsonSchema: {minItems: 10}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {minItems: 10}}}},
                              [{_id: 0}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {minItems: 2}}}},
                              [{_id: 0}, {_id: 2}, {_id: 3}, {_id: 4}]);

    // Test that the JSON Schema fails to parse if "maxItems" is not a valid number.
    assert.throws(() => coll.find({$jsonSchema: {maxItems: "blah"}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {maxItems: -1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {maxItems: 12.5}}).itcount());

    // Test that "maxItems" matches non-arrays, or arrays with at most the given number of items.
    assertFindResultsSortedEq({$jsonSchema: {maxItems: 0}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {maxItems: 0}}}},
                              [{_id: 0}, {_id: 1}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {maxItems: 2}}}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}]);

    // Test that the JSON Schema fails to parse if "uniqueItems" is not a boolean.
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: 1}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: 1.0}}).itcount());
    assert.throws(() => coll.find({$jsonSchema: {uniqueItems: "true"}}).itcount());

    // Test that "uniqueItems" only enforces uniqueness when specified.
    assertFindResultsSortedEq({$jsonSchema: {uniqueItems: true}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {uniqueItems: false}}}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]);
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {uniqueItems: true}}}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}]);

    // TODO (SERVER-30178): uniqueItems should compare objects in a field order-insensitive way when
    // testing for uniqueness.
    assert.writeOK(coll.insert({_id: 5, a: [{x: 1, y: 1}, {y: 1, x: 1}]}));
    assertFindResultsSortedEq({$jsonSchema: {properties: {a: {uniqueItems: true}}}},
                              [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 4}, {_id: 5}]);
}());
