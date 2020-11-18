(function() {
"use strict";

let t = db.slice1;
t.drop();

assert.commandWorked(t.insert({_id: 1, a: [0, 1, 2, 3, 4, 5, -5, -4, -3, -2, -1], b: 1, c: 1}));

// first three
let out = t.findOne({}, {a: {$slice: 3}});
assert.eq(out.a, [0, 1, 2], '1');

// last three
out = t.findOne({}, {a: {$slice: -3}});
assert.eq(out.a, [-3, -2, -1], '2');

// skip 2, limit 3
out = t.findOne({}, {a: {$slice: [2, 3]}});
assert.eq(out.a, [2, 3, 4], '3');

// skip to fifth from last, limit 4
out = t.findOne({}, {a: {$slice: [-5, 4]}});
assert.eq(out.a, [-5, -4, -3, -2], '4');

// skip to fifth from last, limit 10
out = t.findOne({}, {a: {$slice: [-5, 10]}});
assert.eq(out.a, [-5, -4, -3, -2, -1], '5');

// interaction with other fields

out = t.findOne({}, {a: {$slice: 3}});
assert.eq(out.a, [0, 1, 2], 'A 1');
assert.eq(out.b, 1, 'A 2');
assert.eq(out.c, 1, 'A 3');

out = t.findOne({}, {a: {$slice: 3}, b: true});
assert.eq(out.a, [0, 1, 2], 'B 1');
assert.eq(out.b, 1, 'B 2');
assert.eq(out.c, undefined);

out = t.findOne({}, {a: {$slice: 3}, b: false});
assert.eq(out.a, [0, 1, 2]);
assert.eq(out.b, undefined);
assert.eq(out.c, 1);

assert(t.drop());
assert.commandWorked(t.insert({
    comments: [{id: 0, text: 'a'}, {id: 1, text: 'b'}, {id: 2, text: 'c'}, {id: 3, text: 'd'}],
    title: 'foo'
}));

// $slice in an inclusion projection.
out = t.findOne({}, {comments: {$slice: 2}, irrelevantField: 1});
assert.eq(out.comments, [{id: 0, text: 'a'}, {id: 1, text: 'b'}]);
assert.eq(out.title, undefined);

// Check that on its own, $slice defaults to an exclusion projection.
out = t.findOne({}, {comments: {$slice: 2}});
assert.eq(out.comments, [{id: 0, text: 'a'}, {id: 1, text: 'b'}]);
assert.eq(out.title, 'foo');

// $slice in an exclusion projection (with explicit exclusions).
out = t.findOne({}, {comments: {$slice: 2}, title: 0});
assert.eq(out.comments, [{id: 0, text: 'a'}, {id: 1, text: 'b'}]);
assert.eq(out.title, undefined);

// nested arrays
assert(t.drop());
assert.commandWorked(t.insert({_id: 1, a: [[1, 1, 1], [2, 2, 2], [3, 3, 3]], b: 1, c: 1}));

out = t.findOne({}, {a: {$slice: 1}});
assert.eq(out.a, [[1, 1, 1]], 'n 1');

out = t.findOne({}, {a: {$slice: -1}});
assert.eq(out.a, [[3, 3, 3]], 'n 2');

out = t.findOne({}, {a: {$slice: [0, 2]}});
assert.eq(out.a, [[1, 1, 1], [2, 2, 2]], 'n 2');

// Test that $slice operator goes only 1 level deep into nested arrays.
assert(t.drop());
assert.commandWorked(t.insert([
    {_id: 1, a: {b: [1, 2, 3]}},
    {_id: 2, a: [{b: [1, 2, 3]}]},
    {_id: 3, a: [[{b: [1, 2, 3]}]]},
    {_id: 4, a: [[{b: [1, 2, 3]}], {b: [1, 2, 3]}, 1, null, {}]},
]));

out = t.find({}, {a: {b: {$slice: [1, 1]}}}).sort({_id: 1}).toArray();
assert.eq(out, [
    {_id: 1, a: {b: [2]}},
    {_id: 2, a: [{b: [2]}]},
    {_id: 3, a: [[{b: [1, 2, 3]}]]},
    {_id: 4, a: [[{b: [1, 2, 3]}], {b: [2]}, 1, null, {}]},
]);

function testSingleDocument(projection, input, expectedOutput, deleteId = true) {
    assert(t.drop());
    assert.commandWorked(t.insert(input));
    const actualOutput = t.findOne({}, projection);
    if (deleteId) {
        delete actualOutput._id;
    }
    assert.eq(actualOutput, expectedOutput);
}

// Test nesting objects and arrays.
testSingleDocument({'a.b.c.d.e': {$slice: [1, 1]}},
                   {a: [{b: [{c: [{d: [{e: [1, 2, 3]}]}]}]}]},
                   {a: [{b: [{c: [{d: [{e: [2]}]}]}]}]});

// Test that inclusion, exclusion and expression projections have unlimited array depth in queries
// with $slice.
testSingleDocument({a: {b: {c: {$slice: [1, 1]}, d: 1}}},
                   {
                       a: [
                           {b: {c: [1, 2, 3], d: 123}},
                           [{b: {c: [1, 2, 3], d: 123}}, {b: {c: [4, 5, 6], d: 456}}],
                           {b: [{c: [1, 2, 3], d: 123}, {c: [4, 5, 6], d: 456}]},
                           {b: [[{c: [1, 2, 3], d: 123}], [{c: [4, 5, 6], d: 456}]]}
                       ]
                   },
                   {
                       a: [
                           {b: {c: [2], d: 123}},
                           [{b: {c: [1, 2, 3], d: 123}}, {b: {c: [4, 5, 6], d: 456}}],
                           {b: [{c: [2], d: 123}, {c: [5], d: 456}]},
                           {b: [[{c: [1, 2, 3], d: 123}], [{c: [4, 5, 6], d: 456}]]}
                       ]
                   });

testSingleDocument({a: {b: {c: {$slice: [1, 1]}, d: 0}}},
                   {
                       a: [
                           {b: {c: [1, 2, 3], d: 123}},
                           [{b: {c: [1, 2, 3], d: 123}}, {b: {c: [4, 5, 6], d: 456}}],
                           {b: [{c: [1, 2, 3], d: 123}, {c: [4, 5, 6], d: 456}]},
                           {b: [[{c: [1, 2, 3], d: 123}], [{c: [4, 5, 6], d: 456}]]}
                       ]
                   },
                   {
                       a: [
                           {b: {c: [2]}},
                           [{b: {c: [1, 2, 3]}}, {b: {c: [4, 5, 6]}}],
                           {b: [{c: [2]}, {c: [5]}]},
                           {b: [[{c: [1, 2, 3]}], [{c: [4, 5, 6]}]]}
                       ]
                   });

testSingleDocument({a: {b: {c: {$slice: [1, 1]}, d: {$add: [1, 2, 3]}}}},
                   {
                       a: [
                           {b: {c: [1, 2, 3], d: 123}},
                           [{b: {c: [1, 2, 3], d: 123}}, {b: {c: [4, 5, 6], d: 456}}],
                           {b: [{c: [1, 2, 3], d: 123}, {c: [4, 5, 6], d: 456}]},
                           {b: [[{c: [1, 2, 3], d: 123}], [{c: [4, 5, 6], d: 456}]]}
                       ]
                   },
                   {
                       a: [
                           {b: {c: [2], d: 6}},
                           [{b: {c: [1, 2, 3], d: 6}}, {b: {c: [4, 5, 6], d: 6}}],
                           {b: [{c: [2], d: 6}, {c: [5], d: 6}]},
                           {b: [[{c: [1, 2, 3], d: 6}], [{c: [4, 5, 6], d: 6}]]}
                       ]
                   });

testSingleDocument({a: {b: {c: {$slice: [1, 1]}, d: {$add: [{$abs: "$e"}, -4]}}}},
                   {
                       a: [
                           {b: {c: [1, 2, 3], d: 123}},
                           [{b: {c: [1, 2, 3], d: 123}}, {b: {c: [4, 5, 6], d: 456}}],
                           {b: [{c: [1, 2, 3], d: 123}, {c: [4, 5, 6], d: 456}]},
                           {b: [[{c: [1, 2, 3], d: 123}], [{c: [4, 5, 6], d: 456}]]}
                       ],
                       e: -10
                   },
                   {
                       a: [
                           {b: {c: [2], d: 6}},
                           [{b: {c: [1, 2, 3], d: 6}}, {b: {c: [4, 5, 6], d: 6}}],
                           {b: [{c: [2], d: 6}, {c: [5], d: 6}]},
                           {b: [[{c: [1, 2, 3], d: 6}], [{c: [4, 5, 6], d: 6}]]}
                       ]
                   });

// Test multiple $slice operators in the same projection.
testSingleDocument({a: {b: {c: {$slice: [1, 1]}, d: {$slice: -1}}}},
                   {
                       a: [
                           {b: {c: [1, 2, 3], d: [4, 5, 6]}},
                           [
                               {b: {c: [1, 2, 3], d: [4, 5, 6]}},
                               {b: {c: [7, 8, 9], d: [10, 11, 12]}}
                           ],
                           {b: [{c: [1, 2, 3], d: [4, 5, 6]}, {c: [7, 8, 9], d: [10, 11, 12]}]},
                           {b: [[{c: [1, 2, 3], d: [4, 5, 6]}], [{c: [7, 8, 9], d: [10, 11, 12]}]]}
                       ]
                   },
                   {
                       a: [
                           {b: {c: [2], d: [6]}},
                           [
                               {b: {c: [1, 2, 3], d: [4, 5, 6]}},
                               {b: {c: [7, 8, 9], d: [10, 11, 12]}}
                           ],
                           {b: [{c: [2], d: [6]}, {c: [8], d: [12]}]},
                           {b: [[{c: [1, 2, 3], d: [4, 5, 6]}], [{c: [7, 8, 9], d: [10, 11, 12]}]]}
                       ]
                   });

testSingleDocument({a: {b: {$slice: [1, 1]}}, c: {d: {$slice: -1}}},
                   {
                       a: [
                           {b: [1, 2, 3]},
                           [{b: [1, 2, 3]}, {b: [4, 5, 6]}],
                           {b: [[1, 2, 3], [4, 5, 6]]},
                       ],
                       c: [
                           {d: [1, 2, 3]},
                           [{d: [1, 2, 3]}, {d: [4, 5, 6]}],
                           {d: [[1, 2, 3], [4, 5, 6], [7, 8, 9]]},
                       ]
                   },
                   {
                       a: [
                           {b: [2]},
                           [{b: [1, 2, 3]}, {b: [4, 5, 6]}],
                           {b: [[4, 5, 6]]},
                       ],
                       c: [
                           {d: [3]},
                           [{d: [1, 2, 3]}, {d: [4, 5, 6]}],
                           {d: [[7, 8, 9]]},
                       ]
                   });

// Test that if $slice cannot be applied, the field value is still included in the output.
// Case when path for $slice contains object.
testSingleDocument({a: {$slice: 1}}, {a: {c: 123}}, {a: {c: 123}});

testSingleDocument({a: {$slice: 1}, d: 1}, {a: {c: 123}, d: 456}, {a: {c: 123}, d: 456});

testSingleDocument({a: {$slice: 1}, d: 0}, {a: {c: 123}, d: 456}, {a: {c: 123}});

// Case when path for $slice does not exist in the document.
testSingleDocument({a: {$slice: 1}}, {b: {e: 123}}, {b: {e: 123}});

testSingleDocument({a: {$slice: 1}, d: 1}, {b: {e: 123}, d: 456}, {d: 456});

testSingleDocument({a: {$slice: 1}, d: 0}, {b: {e: 123}, d: 456}, {b: {e: 123}});

// Case when path for $slice traverses through deep arrays.
testSingleDocument({'a.b.c': {$slice: 1}},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]]},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]]});

testSingleDocument({'a.b.c': {$slice: 1}, d: 1},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]], d: 456},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]], d: 456});

testSingleDocument({'a.b.c': {$slice: 1}, d: 0},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]], d: 456},
                   {a: [[[{b: [[[{c: [1, 2, 3]}]]]}]]]});

// Test $slice with an inclusion/exclusion projection for _id field.
testSingleDocument({_id: 1, a: {$slice: [1, 1]}},
                   {_id: 123, a: [1, 2, 3]},
                   {_id: 123, a: [2]},
                   false /* deleteId */);

testSingleDocument(
    {_id: 0, a: {$slice: [1, 1]}}, {_id: 123, a: [1, 2, 3]}, {a: [2]}, false /* deleteId */);
})();
