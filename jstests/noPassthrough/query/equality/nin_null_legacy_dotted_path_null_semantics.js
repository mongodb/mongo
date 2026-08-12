// This is a test for the pre-fix behavior described in SERVER-36681 when the
// internalQueryLegacyDottedPathNullSemantics knob is set to true.
// With the fix disabled, a {$nin: [null]} query on a dotted path may return documents
// where the array contains scalars or is empty, because the old path traversal
// code skipped scalar elements without contributing a null match.
// The document set mirrors jstests/core/query/nin/nin_null.js.
// @tags: [
//   requires_fcv_90,
// ]

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

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
    {_id: 20, a: [[[{b: 3}]]]},
    {_id: 21, a: [[{b: 2}], [{b: 3}]]},
    {_id: 22, a: [[{b: 2}], {b: 3}]},
    {_id: 23, a: [{b: 2}, [{b: 3}]]},
];

runWithParamsAllNonConfigNodes(db, {internalQueryLegacyDottedPathNullSemantics: true}, () => {
    assert.commandWorked(coll.insert(docs));
    const allIds = docs.map((doc) => doc._id);

    // "b" doesn't exist as a top-level field on any document, so every document matches null
    // regardless of the knob: there is no array traversal involved.
    assertIds({b: {$nin: [null]}}, []);
    assertIds({b: {$in: [null]}}, allIds);

    // With the fix disabled, only the documents where "a" leads directly to a document or
    // non-array value contribute a null match. Every other document -- where "a" is an empty
    // array, or an array holding scalars, nested arrays, or subdocuments reached through an
    // array -- is returned by {$nin: [null]} and excluded from {$in: [null]}.
    const legacyNinNullIds = [2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23];
    const legacyInNullIds = [0, 1, 8, 9, 10, 11];
    assertIds({"a.b": {$nin: [null]}}, legacyNinNullIds);
    assertIds({"a.b": {$in: [null]}}, legacyInNullIds);

    assert.eq(coll.count({"a.b": {$nin: [null]}}), legacyNinNullIds.length);
    assert.eq(coll.count({"a.b": {$in: [null]}}), legacyInNullIds.length);

    coll.drop();

    const docsWithRealValues = [
        {_id: 0, a: [{b: [[3]]}]},
        {_id: 1, a: {b: [[2]]}},
    ];
    assert.commandWorked(coll.insert(docsWithRealValues));
    const realValueIds = docsWithRealValues.map((doc) => doc._id);

    // "a.b" resolves directly to a non-null leaf value in both documents, so the knob makes no
    // difference here.
    assertIds({"a.b": {$nin: [null]}}, realValueIds);
    assertIds({"a.b": {$in: [null]}}, []);

    assert.eq(coll.count({"a.b": {$nin: [null]}}), realValueIds.length);
    assert.eq(coll.count({"a.b": {$in: [null]}}), 0);
});

MongoRunner.stopMongod(conn);
