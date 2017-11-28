// @tags: [requires_non_retryable_writes]

/**
 * Tests for the array-related behavior of the $type query operator.
 */
(function() {
    "use strict";

    let coll = db.jstest_type_array;
    coll.drop();

    /**
     * Iterates 'cursor' and returns a sorted array of the '_id' fields for the returned documents.
     */
    function extractSortedIdsFromCursor(cursor) {
        let ids = [];
        while (cursor.hasNext()) {
            ids.push(cursor.next()._id);
        }
        return ids.sort();
    }

    function runTests() {
        assert.writeOK(coll.remove({}));
        assert.writeOK(coll.insert({_id: 1, a: [1, 2, 3]}));
        assert.writeOK(coll.insert({_id: 2, a: [1, "foo", 3]}));
        assert.writeOK(coll.insert({_id: 3, a: []}));
        assert.writeOK(coll.insert({_id: 4, a: [[]]}));
        assert.writeOK(coll.insert({_id: 5, a: [[[]]]}));
        assert.writeOK(coll.insert({_id: 6, a: 1}));
        assert.writeOK(coll.insert({_id: 7, a: "foo"}));

        assert.eq([1, 2, 6], extractSortedIdsFromCursor(coll.find({a: {$type: "number"}})));
        assert.eq([2, 7], extractSortedIdsFromCursor(coll.find({a: {$type: "string"}})));
        assert.eq([1, 2, 3, 4, 5], extractSortedIdsFromCursor(coll.find({a: {$type: "array"}})));
        assert.eq([4, 5], extractSortedIdsFromCursor(coll.find({"a.0": {$type: "array"}})));
        assert.eq([5], extractSortedIdsFromCursor(coll.find({"a.0.0": {$type: "array"}})));

        assert.writeOK(coll.remove({}));
        assert.writeOK(coll.insert({_id: 0, a: 1}));
        assert.writeOK(coll.insert({_id: 1, a: NumberInt(1)}));
        assert.writeOK(coll.insert({_id: 2, a: NumberLong(1)}));
        assert.writeOK(coll.insert({_id: 3, a: "str"}));
        assert.writeOK(coll.insert({_id: 4, a: []}));
        assert.writeOK(coll.insert({_id: 5, a: [NumberInt(1), "str"]}));
        assert.writeOK(coll.insert({_id: 6}));

        // Test that $type fails when given array that contains an element that is neither a string
        // nor a number.
        assert.throws(() => coll.find({a: {$type: ["string", null]}}).itcount());
        assert.throws(() => coll.find({a: {$type: [{}, "string"]}}).itcount());

        // Test that $type with an array of types can accept both string aliases and numerical type
        // codes. Also verifies matching behavior for arrays and for missing values.
        assert.eq([2, 3, 5], extractSortedIdsFromCursor(coll.find({a: {$type: ["long", 2]}})));

        // Test $type with an array of types, where one of those types is itself "array".
        assert.eq([2, 4, 5],
                  extractSortedIdsFromCursor(coll.find({a: {$type: ["long", "array"]}})));

        // Test that duplicate types are allowed in the array.
        assert.eq([2, 4, 5],
                  extractSortedIdsFromCursor(
                      coll.find({a: {$type: ["long", "array", "long", "array"]}})));
        assert.eq([2, 4, 5],
                  extractSortedIdsFromCursor(coll.find({a: {$type: ["long", "array", 18, 4]}})));
    }

    // Verify $type queries both with and without an index.
    runTests();
    assert.writeOK(coll.createIndex({a: 1}));
    runTests();
}());
