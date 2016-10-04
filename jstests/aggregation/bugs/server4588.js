// SERVER-4588 Add option to $unwind to emit array index.
(function() {
    "use strict";

    var coll = db.server4588;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0}));
    assert.writeOK(coll.insert({_id: 1, x: null}));
    assert.writeOK(coll.insert({_id: 2, x: []}));
    assert.writeOK(coll.insert({_id: 3, x: [1, 2, 3]}));
    assert.writeOK(coll.insert({_id: 4, x: 5}));

    // Without includeArrayIndex.
    var actualResults = coll.aggregate([{$unwind: {path: "$x"}}]).toArray();
    var expectedResults = [
        {_id: 3, x: 1},
        {_id: 3, x: 2},
        {_id: 3, x: 3},
        {_id: 4, x: 5},
    ];
    assert.eq(expectedResults, actualResults, "Incorrect results for normal $unwind");

    // With includeArrayIndex, index inserted into a new field.
    actualResults = coll.aggregate([{$unwind: {path: "$x", includeArrayIndex: "index"}}]).toArray();
    expectedResults = [
        {_id: 3, x: 1, index: NumberLong(0)},
        {_id: 3, x: 2, index: NumberLong(1)},
        {_id: 3, x: 3, index: NumberLong(2)},
        {_id: 4, x: 5, index: null},
    ];
    assert.eq(expectedResults, actualResults, "Incorrect results $unwind with includeArrayIndex");

    // With both includeArrayIndex and preserveNullAndEmptyArrays.
    // TODO: update this test when SERVER-20168 is resolved.
    actualResults =
        coll.aggregate([{
                $unwind:
                    {path: "$x", includeArrayIndex: "index", preserveNullAndEmptyArrays: true}
            }])
            .toArray();
    expectedResults = [
        {_id: 0, index: null},
        {_id: 1, x: null, index: null},
        {_id: 2, index: null},
        {_id: 3, x: 1, index: NumberLong(0)},
        {_id: 3, x: 2, index: NumberLong(1)},
        {_id: 3, x: 3, index: NumberLong(2)},
        {_id: 4, x: 5, index: null},
    ];
    assert.eq(expectedResults,
              actualResults,
              "Incorrect results $unwind with includeArrayIndex and preserveNullAndEmptyArrays");
}());
