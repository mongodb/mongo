/**
 * Tests DataConsistencyChecker.getDiff() correctly reports mismatched and missing documents.
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
})();
