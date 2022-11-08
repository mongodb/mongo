/**
 * Tests DataConsistencyChecker.getDiff() correctly reports mismatched and missing documents.
 * Tests DataConsistencyChecker.getDiffIndexes() correctly reports mismatched and missing indexes.
 */
(function() {
"use strict";

class ArrayCursor {
    constructor(arr) {
        this.i = 0;
        this.arr = arr;
    }

    hasNext() {
        return this.i < this.arr.length;
    }

    next() {
        return this.arr[this.i++];
    }
}

let diff = DataConsistencyChecker.getDiff(
    new ArrayCursor([{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]),
    new ArrayCursor([{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]));
assert.eq(diff, {
    docsWithDifferentContents: [],
    docsMissingOnFirst: [],
    docsMissingOnSecond: [],
});

diff = DataConsistencyChecker.getDiff(
    new ArrayCursor([{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]),
    new ArrayCursor([{_id: 1, y: 1}, {_id: 2, y: 2}, {_id: 3, y: 3}]));
assert.eq(diff, {
    docsWithDifferentContents: [
        {first: {_id: 1, x: 1}, second: {_id: 1, y: 1}},
        {first: {_id: 2, x: 2}, second: {_id: 2, y: 2}},
        {first: {_id: 3, x: 3}, second: {_id: 3, y: 3}},
    ],
    docsMissingOnFirst: [],
    docsMissingOnSecond: [],
});

diff = DataConsistencyChecker.getDiff(
    new ArrayCursor([{_id: 3, x: 3}]),
    new ArrayCursor([{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]));
assert.eq(diff, {
    docsWithDifferentContents: [],
    docsMissingOnFirst: [{_id: 1, x: 1}, {_id: 2, x: 2}],
    docsMissingOnSecond: [],
});

diff = DataConsistencyChecker.getDiff(
    new ArrayCursor([{_id: 2, x: 2}, {_id: 4, x: 4}]),
    new ArrayCursor([{_id: 1, y: 1}, {_id: 2, y: 2}, {_id: 3, y: 3}]));
assert.eq(diff, {
    docsWithDifferentContents: [{first: {_id: 2, x: 2}, second: {_id: 2, y: 2}}],
    docsMissingOnFirst: [{_id: 1, y: 1}, {_id: 3, y: 3}],
    docsMissingOnSecond: [{_id: 4, x: 4}],
});

diff = DataConsistencyChecker.getDiffIndexes(new ArrayCursor([
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
                                             ]),
                                             new ArrayCursor([
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
                                             ]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [],
    indexesMissingOnFirst: [],
    indexesMissingOnSecond: [],
});

// Order should not matter.
diff = DataConsistencyChecker.getDiffIndexes(new ArrayCursor([
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
                                             ]),
                                             new ArrayCursor([
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}},
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}}
                                             ]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [],
    indexesMissingOnFirst: [],
    indexesMissingOnSecond: [],
});

// Order of fields within index spec should matter.
diff = DataConsistencyChecker.getDiffIndexes(new ArrayCursor([
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
                                             ]),
                                             new ArrayCursor([
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}},
                                                 {name: "a_1_b_1", key: {"a": 1, "b": 1}, v: 2}
                                             ]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [{
        first: {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
        second: {name: "a_1_b_1", key: {"a": 1, "b": 1}, v: 2}
    }],
    indexesMissingOnFirst: [],
    indexesMissingOnSecond: [],
});

// Order of fields within key should matter.
diff = DataConsistencyChecker.getDiffIndexes(new ArrayCursor([
                                                 {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
                                             ]),
                                             new ArrayCursor([
                                                 {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}},
                                                 {name: "a_1_b_1", v: 2, key: {"b": 1, "a": 1}}
                                             ]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [{
        first: {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
        second: {name: "a_1_b_1", v: 2, key: {"b": 1, "a": 1}}
    }],
    indexesMissingOnFirst: [],
    indexesMissingOnSecond: [],
});

// Missing index on first
diff = DataConsistencyChecker.getDiffIndexes(
    new ArrayCursor([{name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}]), new ArrayCursor([
        {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
        {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}},
    ]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [],
    indexesMissingOnFirst: [{name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}}],
    indexesMissingOnSecond: [],
});

// Missing index on second
diff = DataConsistencyChecker.getDiffIndexes(
    new ArrayCursor([
        {name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}},
        {name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}
    ]),
    new ArrayCursor([{name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [],
    indexesMissingOnFirst: [],
    indexesMissingOnSecond: [{name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}}],
});

// Disjoint indexes
diff = DataConsistencyChecker.getDiffIndexes(
    new ArrayCursor([{name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}}]),
    new ArrayCursor([{name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}]));
assert.eq(diff, {
    indexesWithDifferentSpecs: [],
    indexesMissingOnFirst: [{name: "d_1_e_-1", v: 2, key: {"d": 1, "e": -1}}],
    indexesMissingOnSecond: [{name: "a_1_b_1", v: 2, key: {"a": 1, "b": 1}}],
});
})();
