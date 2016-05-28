// SERVER-10176: Add $abs aggregation expression.

// For assertErrorCode.
load('jstests/aggregation/extras/utils.js');

(function() {
    var coll = db.abs_expr;
    coll.drop();

    // valid types (numeric and null)
    assert.writeOK(coll.insert({_id: 0, a: 5}));
    assert.writeOK(coll.insert({_id: 1, a: -5}));
    assert.writeOK(coll.insert({_id: 2, a: 5.5}));
    assert.writeOK(coll.insert({_id: 3, a: -5.5}));
    assert.writeOK(coll.insert({_id: 4, a: NumberInt("5")}));
    assert.writeOK(coll.insert({_id: 5, a: NumberInt("-5")}));
    assert.writeOK(coll.insert({_id: 6, a: NumberLong("5")}));
    assert.writeOK(coll.insert({_id: 7, a: NumberLong("-5")}));
    assert.writeOK(coll.insert({_id: 8, a: 0.0}));
    assert.writeOK(coll.insert({_id: 9, a: -0.0}));
    assert.writeOK(coll.insert({_id: 10, a: NumberInt("0")}));
    // INT_MIN is -(2 ^ 31)
    assert.writeOK(coll.insert({_id: 11, a: NumberInt(-Math.pow(2, 31))}));
    assert.writeOK(coll.insert({_id: 12, a: -Math.pow(2, 31)}));
    // 1152921504606846977 is 2^60 + 1, an integer that can't be represented precisely as a double
    assert.writeOK(coll.insert({_id: 13, a: NumberLong("1152921504606846977")}));
    assert.writeOK(coll.insert({_id: 14, a: NumberLong("-1152921504606846977")}));
    assert.writeOK(coll.insert({_id: 15, a: null}));
    assert.writeOK(coll.insert({_id: 16, a: undefined}));
    assert.writeOK(coll.insert({_id: 17, a: NaN}));
    assert.writeOK(coll.insert({_id: 18}));

    // valid use of $abs: numbers become positive, null/undefined/nonexistent become null

    var results = coll.aggregate([{$project: {a: {$abs: "$a"}}}]).toArray();
    assert.eq(results, [
        {_id: 0, a: 5},
        {_id: 1, a: 5},
        {_id: 2, a: 5.5},
        {_id: 3, a: 5.5},
        {_id: 4, a: 5},
        {_id: 5, a: 5},
        {_id: 6, a: NumberLong("5")},
        {_id: 7, a: NumberLong("5")},
        {_id: 8, a: 0},
        {_id: 9, a: 0},
        {_id: 10, a: 0},
        {_id: 11, a: NumberLong(Math.pow(2, 31))},
        {_id: 12, a: Math.pow(2, 31)},
        {_id: 13, a: NumberLong("1152921504606846977")},
        {_id: 14, a: NumberLong("1152921504606846977")},
        {_id: 15, a: null},
        {_id: 16, a: null},
        {_id: 17, a: NaN},
        {_id: 18, a: null},
    ]);
    // Invalid

    // using $abs on string
    assertErrorCode(coll, [{$project: {a: {$abs: "string"}}}], 28765);

    // using $abs on LLONG_MIN (-2 ^ 63)
    assertErrorCode(coll, [{$project: {a: {$abs: NumberLong("-9223372036854775808")}}}], 28680);
}());
