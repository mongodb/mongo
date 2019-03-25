// Tests basic use cases for all $merge modes.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq.

    // Asserts that two arrays are equal - that is, if their sizes are equal and each element in
    // the 'actual' array has a matching element in the 'expected' array, without honoring
    // elements order.
    function assertArrayEq({actual = [], expected = []} = {}) {
        assert(arrayEq(actual, expected), `actual=${tojson(actual)}, expected=${tojson(expected)}`);
    }

    const source = db.all_modes_source;
    const target = db.all_modes_target;

    (function setup() {
        source.drop();
        target.drop();

        // All tests use the same data in the source collection.
        assert.commandWorked(source.insert(
            [{_id: 1, a: 1, b: "a"}, {_id: 2, a: 2, b: "b"}, {_id: 3, a: 3, b: "c"}]));

    })();

    // Test 'whenMatched=fail whenNotMatched=insert' mode. This is an equivalent of a
    // replacemnt-style update with upsert=true.
    (function testWhenMatchedReplaceWithNewWhenNotMatchedInsert() {
        assert.commandWorked(target.insert([{_id: 1, a: 10}, {_id: 3, a: 30}, {_id: 4, a: 40}]));
        assert.doesNotThrow(() => source.aggregate([{
            $merge: {
                into: target.getName(),
                whenMatched: "replaceWithNew",
                whenNotMatched: "insert"
            }
        }]));
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, a: 1, b: "a"},
                {_id: 2, a: 2, b: "b"},
                {_id: 3, a: 3, b: "c"},
                {_id: 4, a: 40}
            ]
        });
    })();

    // Test 'whenMatched=fail whenNotMatched=insert' mode. For matched documents the update should
    // be unordered and report an error at the end when all documents in a batch have been
    // processed, it will not fail as soon as we hit the first document without a match.
    (function testWhenMatchedFailWhenNotMatchedInsert() {
        assert(target.drop());
        assert.commandWorked(target.insert(
            [{_id: 10, a: 10, c: "x"}, {_id: 3, a: 30, c: "y"}, {_id: 4, a: 40, c: "z"}]));
        const error = assert.throws(() => source.aggregate([
            {$merge: {into: target.getName(), whenMatched: "fail", whenNotMatched: "insert"}}
        ]));
        assert.commandFailedWithCode(error, ErrorCodes.DuplicateKey);
        assertArrayEq({
            actual: target.find().toArray(),
            expected: [
                {_id: 1, a: 1, b: "a"},
                {_id: 2, a: 2, b: "b"},
                {_id: 3, a: 30, c: "y"},
                {_id: 4, a: 40, c: "z"},
                {_id: 10, a: 10, c: "x"}
            ]
        });
    })();
}());
