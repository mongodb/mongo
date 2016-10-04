// SERVER-23029 added a new expression, $reverseArray, which consumes an array or a nullish value
// and produces either the reversed version of that array, or null. In this test file, we check the
// behavior and error cases.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.reverseArray;
    coll.drop();

    // We need a document to flow through the pipeline, even though we don't care what fields it
    // has.
    coll.insert({});

    assertErrorCode(coll, [{$project: {reversed: {$reverseArray: 1}}}], 34435);

    var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [1, 2]}}}}]);
    var output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].reversed, [2, 1]);

    var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [[1, 2]]}}}}]);
    var output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].reversed, [[1, 2]]);

    var res = coll.aggregate([{$project: {reversed: {$reverseArray: "$notAField"}}}]);
    var output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].reversed, null);
}());
