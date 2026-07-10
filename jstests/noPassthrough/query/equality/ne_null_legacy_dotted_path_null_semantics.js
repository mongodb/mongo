// This is a test for the pre-fix behavior described in SERVER-36681 when the
// internalQueryLegacyDottedPathNullSemantics knob is set to true.
// With the fix disabled, a {$ne: null} query on a dotted path may return documents
// where the array contains scalars or is empty, because the old path traversal
// code skipped scalar elements without contributing a null match.
// @tags: [
//   requires_fcv_90,
// ]

import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const coll = db[jsTestName()];
coll.drop();

runWithParamsAllNonConfigNodes(db, {internalQueryLegacyDottedPathNullSemantics: true}, () => {
    assert.commandWorked(coll.insert({a: [1, {c: 1}]}));
    assert.commandWorked(coll.insert({a: [1]}));
    assert.commandWorked(coll.insert({a: []}));
    assert.commandWorked(coll.insert({a: [[]]}));
    assert.commandWorked(coll.insert({a: [{}]}));
    assert.commandWorked(coll.insert({}));
    assert.commandWorked(coll.insert({a: {}}));
    assert.commandWorked(coll.insert({a: null}));

    assert.eq(coll.count({b: {$ne: null}}), 0);
    // With fix disabled, documents where 'a' is an empty array, contains only scalars,
    // or contains a nested empty array are incorrectly returned by {$ne: null}.
    assert.eq(coll.count({"a.b": {$ne: null}}), 3);

    assert.eq(coll.count({b: {$eq: null}}), 8);
    assert.eq(coll.count({"a.b": {$eq: null}}), 5);
});

MongoRunner.stopMongod(conn);
