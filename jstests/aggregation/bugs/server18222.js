// SERVER-18222: Add $isArray aggregation expression.
(function() {
    'use strict';
    var coll = db.is_array_expr;
    coll.drop();

    // Non-array types.
    assert.writeOK(coll.insert({_id: 0, x: 0}));
    assert.writeOK(coll.insert({_id: 1, x: '0'}));
    assert.writeOK(coll.insert({_id: 2, x: new ObjectId()}));
    assert.writeOK(coll.insert({_id: 3, x: new NumberLong(0)}));
    assert.writeOK(coll.insert({_id: 4, x: {y: []}}));
    assert.writeOK(coll.insert({_id: 5, x: null}));
    assert.writeOK(coll.insert({_id: 6, x: NaN}));
    assert.writeOK(coll.insert({_id: 7, x: undefined}));

    // Array types.
    assert.writeOK(coll.insert({_id: 8, x: []}));
    assert.writeOK(coll.insert({_id: 9, x: [0]}));
    assert.writeOK(coll.insert({_id: 10, x: ['0']}));

    // Project field is_array to represent whether the field x was an array.
    var results = coll.aggregate([
                          {$sort: {_id: 1}},
                          {$project: {isArray: {$isArray: '$x'}}},
                      ])
                      .toArray();
    var expectedResults = [
        {_id: 0, isArray: false},
        {_id: 1, isArray: false},
        {_id: 2, isArray: false},
        {_id: 3, isArray: false},
        {_id: 4, isArray: false},
        {_id: 5, isArray: false},
        {_id: 6, isArray: false},
        {_id: 7, isArray: false},
        {_id: 8, isArray: true},
        {_id: 9, isArray: true},
        {_id: 10, isArray: true},
    ];

    assert.eq(results, expectedResults);
}());
