// This is a test for the query correctness bug described in SERVER-36681. A {$ne: null} query
// should not return documents where the value doesn't exist
// @tags: [
//   # SERVER-36681 changed the behavior of SBE and classic engines
//   requires_fcv_90,
// ]

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];

function assertIds(query, expectedIds) {
    const resultIds = coll.find(query, {_id: 1}).map((d) => d._id);
    assertArrayEq({actual: resultIds, expected: expectedIds});
}

coll.drop();

const docs = [
    {_id: 0, a: [1, {c: 1}]},
    {_id: 1, a: 1},
    {_id: 2, a: [1]},
    {_id: 3, a: [[1]]},
    {_id: 4, a: [[[1]]]},
    {_id: 5, a: []},
    {_id: 6, a: [[]]},
    {_id: 7, a: [[[]]]},
    {_id: 8, a: [{}]},
    {_id: 9},
    {_id: 10, a: {}},
    {_id: 11, a: null},
    {_id: 12, a: [null]},
    {_id: 13, a: [[null]]},
    {_id: 14, a: [[], {b: 3}]},
    {_id: 15, a: [[2], {b: 3}]},
    {_id: 16, a: [[{b: 3}], 4]},
    {_id: 17, a: [[[{c: 3}]]]},
    {_id: 18, a: [[[{b: null}]]]},
    {_id: 19, a: [[[{}]]]},
    // These are counter-intuitive but since we don't recursively traverse into arrays, the
    // following also match null: the value nested inside is never reached.
    {_id: 20, a: [[[{b: 3}]]]},
    {_id: 21, a: [[{b: 2}], [{b: 3}]]},
    {_id: 22, a: [[{b: 2}], {b: 3}]},
    {_id: 23, a: [{b: 2}, [{b: 3}]]},
];
assert.commandWorked(coll.insert(docs));
const allIds = docs.map((doc) => doc._id);

// "b" doesn't exist as a top-level field on any document, so every document matches null.
assertIds({b: {$ne: null}}, []);
assertIds({b: {$eq: null}}, allIds);

// None of the documents above have a genuinely reachable, non-null value at "a.b", so every
// document matches null there as well.
assertIds({"a.b": {$ne: null}}, []);
assertIds({"a.b": {$eq: null}}, allIds);

assert.eq(coll.count({"a.b": {$eq: null}}), allIds.length);
assert.eq(coll.count({"a.b": {$ne: null}}), 0);

coll.drop();

const docsWithRealValues = [
    {_id: 0, a: [{b: [[3]]}]},
    {_id: 1, a: {b: [[2]]}},
];
assert.commandWorked(coll.insert(docsWithRealValues));
const realValueIds = docsWithRealValues.map((doc) => doc._id);

// Here "a.b" resolves directly to a non-null leaf value in both documents.
assertIds({"a.b": {$ne: null}}, realValueIds);
assertIds({"a.b": {$eq: null}}, []);

assert.eq(coll.count({"a.b": {$ne: null}}), realValueIds.length);
assert.eq(coll.count({"a.b": {$eq: null}}), 0);
