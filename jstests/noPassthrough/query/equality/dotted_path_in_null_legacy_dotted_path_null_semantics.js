// Tests the pre-fix behavior of dotted-path null queries when
// internalQueryLegacyDottedPathNullSemantics is true (SERVER-36681).
// @tags: [
//   requires_fcv_90,
// ]
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

const coll = db.dotted_path_in_null_disable_fix;
coll.drop();

runWithParamsAllNonConfigNodes(db, {internalQueryLegacyDottedPathNullSemantics: true}, () => {
    assert.commandWorked(coll.insert({_id: 1, a: [{b: 5}]}));
    assert.commandWorked(coll.insert({_id: 2, a: [{}]}));
    assert.commandWorked(coll.insert({_id: 3, a: []}));
    assert.commandWorked(coll.insert({_id: 4, a: [{}, {b: 5}]}));
    assert.commandWorked(coll.insert({_id: 5, a: [5, {b: 5}]}));

    function getIds(query) {
        let ids = [];
        coll.find(query)
            .sort({_id: 1})
            .forEach((doc) => ids.push(doc._id));
        return ids;
    }

    // With the fix disabled, documents where 'a' is an empty array or contains only scalars
    // are not matched by a null predicate on the dotted path 'a.b'.
    assert.eq([2, 4], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");

    assert.commandWorked(coll.createIndex({"a.b": 1}));
    assert.eq([2, 4], getIds({"a.b": {$in: [null]}}), "Did not match the expected documents");
});

MongoRunner.stopMongod(conn);
