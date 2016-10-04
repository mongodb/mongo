// SERVER-20163 introduced the $zip expression. In this test file, we check the behavior and error
// cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

(function() {
    "use strict";

    var coll = db.zip;
    coll.drop();

    coll.insert({'long': [1, 2, 3], 'short': ['x', 'y']});

    var zipObj = 3;
    assertErrorCode(coll,
                    [{$project: {zipped: {$zip: zipObj}}}],
                    34460,
                    "$zip requires an object" + " as an argument.");

    zipObj = {inputs: []};
    assertErrorCode(coll,
                    [{$project: {zipped: {$zip: zipObj}}}],
                    34465,
                    "$zip requires at least" + " one input array");

    zipObj = {inputs: {"a": "b"}};
    assertErrorCode(coll, [{$project: {zipped: {$zip: zipObj}}}], 34461, "inputs is not an array");

    zipObj = {inputs: ["$a"], defaults: ["A"]};
    assertErrorCode(coll,
                    [{$project: {zipped: {$zip: zipObj}}}],
                    34466,
                    "cannot specify defaults" + " unless useLongestLength is true.");

    zipObj = {inputs: ["$a"], defaults: ["A", "B"], useLongestLength: true};
    assertErrorCode(coll,
                    [{$project: {zipped: {$zip: zipObj}}}],
                    34467,
                    "inputs and defaults" + " must be the same length.");

    zipObj = {inputs: ["$a"], defaults: {"a": "b"}};
    assertErrorCode(
        coll, [{$project: {zipped: {$zip: zipObj}}}], 34462, "defaults is not an" + " array");

    zipObj = {inputs: ["$a"], defaults: ["A"], useLongestLength: 1};
    assertErrorCode(
        coll, [{$project: {zipped: {$zip: zipObj}}}], 34463, "useLongestLength is not" + " a bool");

    zipObj = {inputs: ["$a", "$b"], defaults: ["A"], notAField: 1};
    assertErrorCode(coll, [{$project: {zipped: {$zip: zipObj}}}], 34464, "unknown argument");

    zipObj = {inputs: ["A", "B"]};
    assertErrorCode(coll,
                    [{$project: {zipped: {$zip: zipObj}}}],
                    34468,
                    "an element of inputs" + " was not an array.");

    zipObj = {inputs: [[1, 2, 3], ["A", "B", "C"]]};
    var res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    var output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[1, "A"], [2, "B"], [3, "C"]]);

    zipObj = {inputs: [[1, 2, 3], null]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, null);

    zipObj = {inputs: [null, [1, 2, 3]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, null);

    zipObj = {inputs: ["$missing", [1, 2, 3]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, null);

    zipObj = {inputs: [undefined, [1, 2, 3]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, null);

    zipObj = {inputs: [[1, 2, 3], ["A", "B"]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[1, "A"], [2, "B"]]);

    zipObj = {inputs: [["A", "B"], [1, 2, 3]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [["A", 1], ["B", 2]]);

    zipObj = {inputs: [[], []]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, []);

    zipObj = {inputs: [["$short"], ["$long"]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[['x', 'y'], [1, 2, 3]]]);

    zipObj = {inputs: ["$short", "$long"]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [['x', 1], ['y', 2]]);

    zipObj = {inputs: [["$long"]]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[[1, 2, 3]]]);

    zipObj = {inputs: [[1, 2, 3], ['a', 'b', 'c'], ['c', 'b', 'a']]};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[1, 'a', 'c'], [2, 'b', 'b'], [3, 'c', 'a']]);

    zipObj = {inputs: [[1, 2, 3], ["A", "B"]], defaults: ["C", "D"], useLongestLength: true};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[1, "A"], [2, "B"], [3, "D"]]);

    zipObj = {inputs: [[1, 2, 3], ["A", "B"]], useLongestLength: true};
    res = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]);
    output = res.toArray();
    assert.eq(1, output.length);
    assert.eq(output[0].zipped, [[1, "A"], [2, "B"], [3, null]]);
}());
