// SERVER-4588 Add option to $unwind to emit array index.
(function() {
    "use strict";

    var coll = db.server4588;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0}));
    assert.writeOK(coll.insert({_id: 1, x: null}));
    assert.writeOK(coll.insert({_id: 2, x: []}));
    assert.writeOK(coll.insert({_id: 3, x: [1, 2, 3]}));

    // Without includeArrayIndex.
    var actualResults = coll.aggregate([{$unwind: {path: "$x"}}]).toArray();
    var expectedResults = [
        {_id: 3, x: 1},
        {_id: 3, x: 2},
        {_id: 3, x: 3},
    ];
    assert.eq(expectedResults, actualResults, "Incorrect results for normal $unwind");

    // With includeArrayIndex.
    actualResults = coll.aggregate([
        {$unwind: {path: "$x", includeArrayIndex: true}}
    ]).toArray();
    expectedResults = [
        {_id: 3, x: {index: NumberLong(0), value: 1}},
        {_id: 3, x: {index: NumberLong(1), value: 2}},
        {_id: 3, x: {index: NumberLong(2), value: 3}},
    ];
    assert.eq(expectedResults, actualResults, "Incorrect results $unwind with includeArrayIndex");

    // With both includeArrayIndex and preserveNullAndEmptyArrays.
    actualResults = coll.aggregate([
        {$unwind: {path: "$x", includeArrayIndex: true, preserveNullAndEmptyArrays: true}}
    ]).toArray();
    expectedResults = [
        {_id: 0},
        {_id: 1, x: null},
        {_id: 2, x: []},
        {_id: 3, x: {index: NumberLong(0), value: 1}},
        {_id: 3, x: {index: NumberLong(1), value: 2}},
        {_id: 3, x: {index: NumberLong(2), value: 3}},
    ];
    assert.eq(expectedResults,
              actualResults,
              "Incorrect results $unwind with includeArrayIndex and preserveNullAndEmptyArrays");
}());
