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
        assert.eq([1, 2, 6], extractSortedIdsFromCursor(coll.find({a: {$type: "number"}})));
        assert.eq([2, 7], extractSortedIdsFromCursor(coll.find({a: {$type: "string"}})));
        assert.eq([1, 2, 3, 4, 5], extractSortedIdsFromCursor(coll.find({a: {$type: "array"}})));
        assert.eq([4, 5], extractSortedIdsFromCursor(coll.find({"a.0": {$type: "array"}})));
        assert.eq([5], extractSortedIdsFromCursor(coll.find({"a.0.0": {$type: "array"}})));
    }

    assert.writeOK(coll.insert({_id: 1, a: [1, 2, 3]}));
    assert.writeOK(coll.insert({_id: 2, a: [1, "foo", 3]}));
    assert.writeOK(coll.insert({_id: 3, a: []}));
    assert.writeOK(coll.insert({_id: 4, a: [[]]}));
    assert.writeOK(coll.insert({_id: 5, a: [[[]]]}));
    assert.writeOK(coll.insert({_id: 6, a: 1}));
    assert.writeOK(coll.insert({_id: 7, a: "foo"}));

    // Verify $type queries both with and without an index.
    runTests();
    assert.writeOK(coll.createIndex({a: 1}));
    runTests();
}());
