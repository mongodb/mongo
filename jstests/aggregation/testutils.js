// Tests the test utilities themselves.
(function() {
load("jstests/aggregation/extras/utils.js");

const verbose = false;

const example = [
    {_id: ObjectId("4dc07fedd8420ab8d0d4066d"), pageViews: 5, tags: ["fun", "good"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066e"), pageViews: 7, tags: ["fun", "nasty"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066f"), pageViews: 6, tags: ["nasty", "filthy"]}
];

assert(arrayEq(example, example, verbose));
assert(resultsEq(example, example, verbose));

const exampleDifferentOrder = [
    {_id: ObjectId("4dc07fedd8420ab8d0d4066d"), pageViews: 5, tags: ["fun", "good"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066f"), pageViews: 6, tags: ["nasty", "filthy"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066e"), pageViews: 7, tags: ["fun", "nasty"]},
];

assert(resultsEq(exampleDifferentOrder, example, verbose));
assert(resultsEq(example, exampleDifferentOrder, verbose));
assert(!orderedArrayEq(example, exampleDifferentOrder, verbose));

const exampleFewerEntries = [
    {_id: ObjectId("4dc07fedd8420ab8d0d4066e"), pageViews: 7, tags: ["fun", "nasty"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066f"), pageViews: 6, tags: ["nasty", "filthy"]}
];

assert(!resultsEq(example, exampleFewerEntries, verbose));
assert(!resultsEq(exampleFewerEntries, example, verbose));

const exampleNoIds = [
    {pageViews: 5, tags: ["fun", "good"]},
    {pageViews: 7, tags: ["fun", "nasty"]},
    {pageViews: 6, tags: ["nasty", "filthy"]}
];

assert(!resultsEq(example, exampleNoIds, verbose));
assert(!resultsEq(exampleNoIds, example, verbose));

const exampleMissingTags = [
    {_id: ObjectId("4dc07fedd8420ab8d0d4066d"), pageViews: 5, tags: ["fun"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066e"), pageViews: 7, tags: ["fun", "nasty"]},
    {_id: ObjectId("4dc07fedd8420ab8d0d4066f"), pageViews: 6, tags: ["filthy"]}
];

assert(!resultsEq(example, exampleMissingTags, verbose));
assert(!resultsEq(exampleMissingTags, example, verbose));

const exampleDifferentIds = [
    {_id: 0, pageViews: 5, tags: ["fun", "good"]},
    {_id: 1, pageViews: 7, tags: ["fun", "nasty"]},
    {_id: 2, pageViews: 6, tags: ["nasty", "filthy"]}
];
assertArrayEq({actual: example, expected: exampleDifferentIds, fieldsToSkip: ["_id"]});
assertArrayEq({actual: exampleDifferentIds, expected: example, fieldsToSkip: ["_id"]});

// Test using a custom comparator.
assert(customDocumentEq({
    left: {a: 1, b: 3},
    right: {a: "ignore", b: 3},
    verbose: verbose,
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));
assert(!customDocumentEq({
    left: {a: 1, b: 3},
    right: {a: 3, b: 3},
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));

// Test using a custom comparator with arrays.
assert(customDocumentEq({
    left: {a: [1, 2], b: 3},
    right: {a: [2, "ignore"], b: 3},
    verbose: verbose,
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));
assert(!customDocumentEq({
    left: {a: [1, 2], b: 3},
    right: {a: [3, "ignore"], b: 3},
    verbose: verbose,
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));

// Test using a custom comparator with arrays of objects.
assert(customDocumentEq({
    left: {a: [{b: 1}, {b: 2}, {b: 3}]},
    right: {a: [{b: "ignore"}, {b: 2}, {b: 3}]},
    verbose: verbose,
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));
assert(!customDocumentEq({
    left: {a: [{b: 1}, {b: 2}, {b: 1}]},
    right: {a: [{b: "ignore"}, {b: 2}, {b: 3}]},
    verbose: verbose,
    valueComparator: (l, r) => {
        if (l == "ignore" || r == "ignore") {
            return true;
        }
        return l == r;
    }
}));

assert(!anyEq(5, [5], verbose));
assert(!anyEq([5], 5, verbose));
assert(!anyEq("5", 5, verbose));
assert(!anyEq(5, "5", verbose));

assert(arrayEq([{c: 6}, [5], [4, 5], 2, undefined, 3, null, 4, 5],
               [undefined, null, 2, 3, 4, 5, {c: 6}, [4, 5], [5]],
               verbose));

assert(arrayEq([undefined, null, 2, 3, 4, 5, {c: 6}, [4, 5], [5]],
               [{c: 6}, [5], [4, 5], 2, undefined, 3, null, 4, 5],
               verbose));

// Tests for the difference between arrayEq() and assert.sameMembers() : nested array order is not
// significant in the first and significant in the latter.
assert(arrayEq([1, [2, 3, 4]], [[4, 3, 2], 1]));
assert.throws(() => assert.sameMembers([1, [2, 3, 4]], [[4, 3, 2], 1]));

// Tests for the difference between orderedArrayEq() and assert.eq(): element order is significant
// only at the top-level in orderedArrayEq() and always in assert.eq().
assert(orderedArrayEq([1, [2, 3, 4]], [1, [2, 3, 4]], verbose));
assert(orderedArrayEq([1, [2, 3, 4]], [1, [4, 3, 2]], verbose));
assert.eq([1, [2, 3, 4]], [1, [2, 3, 4]]);
assert.neq([1, [2, 3, 4]], [1, [4, 3, 2]]);

// Tests for the difference between documentEq() and assert.docEq(): element order is not
// significant in nested array values and property values can be skipped in documentEq().
assert(documentEq({a: [1, 2, 3], b: 4, c: 5}, {a: [3, 2, 1], c: 5, b: 4}));

assert.docEq({a: [1, 2, 3], b: 4, c: 5}, {a: [1, 2, 3], c: 5, b: 4});
assert.throws(() => assert.docEq({a: [1, 2, 3], b: 4, c: 5}, {a: [3, 2, 1], c: 5, b: 4}));

assert(documentEq({a: [1, 2, 3], b: 4, c: null},
                  {a: [10, 20], c: 5, b: 4},
                  false /* verbose */,
                  null /* valueComparator */,
                  ["a", "c"]));
}());
