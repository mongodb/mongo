// Basic testing for the $unset aggregation stage.
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

    const coll = db.agg_stage_unset;
    coll.drop();

    assert.commandWorked(coll.insert(
        [{_id: 0, a: 10}, {_id: 1, a: {b: 20, c: 30, 0: 40}}, {_id: 2, a: [{b: 50, c: 60}]}]));

    // unset single field.
    let result = coll.aggregate([{$unset: ["a"]}]).toArray();
    assertArrayEq({actual: result, expected: [{_id: 0}, {_id: 1}, {_id: 2}]});

    // unset should work with string directive.
    result = coll.aggregate([{$unset: "a"}]).toArray();
    assertArrayEq({actual: result, expected: [{_id: 0}, {_id: 1}, {_id: 2}]});

    // unset multiple fields.
    result = coll.aggregate([{$unset: ["_id", "a"]}]).toArray();
    assertArrayEq({actual: result, expected: [{}, {}, {}]});

    // unset with dotted field path.
    result = coll.aggregate([{$unset: ["a.b"]}]).toArray();
    assertArrayEq({
        actual: result,
        expected: [{_id: 0, a: 10}, {_id: 1, a: {0: 40, c: 30}}, {_id: 2, a: [{c: 60}]}]
    });

    // Numeric field paths in aggregation represent field name only and not array offset.
    result = coll.aggregate([{$unset: ["a.0"]}]).toArray();
    assertArrayEq({
        actual: result,
        expected: [{_id: 0, a: 10}, {_id: 1, a: {b: 20, c: 30}}, {_id: 2, a: [{b: 50, c: 60}]}]
    });

})();
