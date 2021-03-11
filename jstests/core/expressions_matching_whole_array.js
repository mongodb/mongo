// Test MQL expressions which can match against the whole array stored in the field.
(function() {
"use strict";

const coll = db.expressions_matching_whole_array;
coll.drop();

function testSingleDocument(document, query, shouldMatch) {
    assert.commandWorked(coll.insert(document));

    const result = coll.find(query).toArray();
    if (shouldMatch) {
        assert.eq(result.length, 1);
        delete result[0]._id;
        assert.docEq(document, result[0]);
    } else {
        assert.eq(result.length, 0);
    }

    assert(coll.drop());
}

function assertMatches(document, query) {
    testSingleDocument(document, query, true);
}

function assertDoesNotMatch(document, query) {
    testSingleDocument(document, query, false);
}

// $type
assertMatches({a: [1, 2, 3]}, {a: {$type: 'array'}});
assertMatches({a: []}, {a: {$type: 'array'}});
assertDoesNotMatch({a: 1}, {a: {$type: 'array'}});

// $in
assertMatches({a: [1, 2, 3]}, {a: {$in: [[1, 2, 3]]}});
assertDoesNotMatch({a: [1, 2, 3]}, {a: {$in: [[1, 2]]}});

assertMatches({a: []}, {a: {$in: [[]]}});
assertDoesNotMatch({a: []}, {a: {$in: [[1]]}});

// $where matches ONLY against whole arrays.
assertMatches({a: [1]}, {$where: 'Array.isArray(this.a) && this.a.length == 1 && this.a[0] == 1'});
assertDoesNotMatch({a: [1, 2, 3]}, {$where: 'this.a == 1'});

// Comparison expressions match whole array only when RHS has array, MaxKey or MinKey types.
assertMatches({a: [1, 2, 3]}, {a: [1, 2, 3]});
assertDoesNotMatch({a: [1, 2, 3]}, {a: [1, 2]});

assertMatches({a: [1, 2, 3]}, {a: {$gt: MinKey}});
assertMatches({a: []}, {a: {$gt: MinKey}});

assertMatches({a: [1, 2, 3]}, {a: {$lt: MaxKey}});
assertMatches({a: []}, {a: {$lt: MaxKey}});
}());