// Tests the test utilities themselves.
(function() {
load("jstests/aggregation/extras/utils.js");

const verbose = false;

/******************************* Tests anyEq *********************************/
// Check Mongo shell types work properly with anyEq.
assert(!anyEq({a: NumberLong(1)}, {a: NumberLong(5)}, verbose));
assert(anyEq({a: NumberLong(1)}, {a: NumberLong(1)}, verbose));

assert(!anyEq({a: NumberInt(1)}, {a: NumberInt(5)}, verbose));
assert(anyEq({a: NumberInt(1)}, {a: NumberInt(1)}, verbose));

assert(!anyEq({a: NumberDecimal("1.0")}, {a: NumberDecimal("5.0")}, verbose));
assert(anyEq({a: NumberDecimal("1.0")}, {a: NumberDecimal("1.0")}, verbose));
assert(anyEq(NumberDecimal("0.1"), NumberDecimal("0.100"), verbose));

assert(!anyEq({a: new Date(Date.UTC(1984, 0, 1))}, {a: new Date(Date.UTC(1990, 0, 1))}, verbose));
assert(anyEq({a: new Date(Date.UTC(1984, 0, 1))}, {a: new Date(Date.UTC(1984, 0, 1))}, verbose));

assert(!anyEq(
    {a: ObjectId("4dc07fedd8420ab8d0d4066d")}, {a: ObjectId("4dc07fedd8420ab8d0d4066e")}, verbose));
assert(anyEq(
    {a: ObjectId("4dc07fedd8420ab8d0d4066d")}, {a: ObjectId("4dc07fedd8420ab8d0d4066d")}, verbose));

assert(!anyEq({a: new Timestamp(1, 0)}, {a: new Timestamp(0, 0)}, verbose));
assert(anyEq({a: new Timestamp(0, 1)}, {a: new Timestamp(0, 1)}, verbose));

// Other basic anyEq tests.
assert(!anyEq(5, [5], verbose));
assert(!anyEq([5], 5, verbose));
assert(!anyEq("5", 5, verbose));
assert(!anyEq(5, "5", verbose));
assert(anyEq({a: []}, {a: []}, verbose));
assert(!anyEq({"a": {}}, {"a": []}, verbose));
assert(anyEq(undefined, undefined, verbose));
assert(!anyEq({}, undefined, verbose));

/******************************* Tests documentEq *********************************/
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

/*********************** Tests arrayEq, orderedArrayEq, resultsEq ***********************/
// Check Mongo shell types work properly with arrayEq.
assert(!arrayEq([{a: NumberLong(1)}], [{a: NumberLong(5)}], verbose));
assert(arrayEq([{a: NumberLong(1)}], [{a: NumberLong(1)}], verbose));

assert(!arrayEq([{a: NumberInt(1)}], [{a: NumberInt(5)}], verbose));
assert(arrayEq([{a: NumberInt(1)}], [{a: NumberInt(1)}], verbose));

assert(!arrayEq([{a: NumberDecimal("1.0")}], [{a: NumberDecimal("5.0")}], verbose));
assert(arrayEq([{a: NumberDecimal("1.0")}], [{a: NumberDecimal("1.0")}], verbose));
assert(arrayEq([NumberDecimal("0.1")], [NumberDecimal("0.100")], verbose));

assert(!arrayEq(
    [{a: new Date(Date.UTC(1984, 0, 1))}], [{a: new Date(Date.UTC(1990, 0, 1))}], verbose));
assert(
    arrayEq([{a: new Date(Date.UTC(1984, 0, 1))}], [{a: new Date(Date.UTC(1984, 0, 1))}], verbose));

assert(!arrayEq([{a: ObjectId("4dc07fedd8420ab8d0d4066d")}],
                [{a: ObjectId("4dc07fedd8420ab8d0d4066e")}],
                verbose));
assert(arrayEq([{a: ObjectId("4dc07fedd8420ab8d0d4066d")}],
               [{a: ObjectId("4dc07fedd8420ab8d0d4066d")}],
               verbose));

assert(!arrayEq([{a: new Timestamp(1, 0)}], [{a: new Timestamp(0, 0)}], verbose));
assert(arrayEq([{a: new Timestamp(0, 1)}], [{a: new Timestamp(0, 1)}], verbose));

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
}());
