// Test indexed elemmatch of missing field with internalQueryLegacyDottedPathNullSemantics=true.
// @tags: [
//   requires_getmore,
//   requires_fcv_90,
// ]
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

runWithParamsAllNonConfigNodes(db, {internalQueryLegacyDottedPathNullSemantics: true}, () => {
    let t = db.jstests_arrayfind5_disable_fix;
    t.drop();

    function check(nullElemMatch) {
        assert.eq(1, t.find({"a.b": 1}).itcount());
        assert.eq(1, t.find({a: {$elemMatch: {b: 1}}}).itcount());
        // With fix disabled, documents where 'a' contains scalars are not matched by {a.b: null}.
        assert.eq(nullElemMatch ? 1 : 0, t.find({"a.b": null}).itcount());
        assert.eq(nullElemMatch ? 1 : 0, t.find({a: {$elemMatch: {b: null}}}).itcount()); // see SERVER-3377
    }

    t.save({a: [{}, {b: 1}]});
    check(true);
    t.createIndex({"a.b": 1});
    check(true);

    t.drop();

    t.save({a: [5, {b: 1}]});
    check(false);
    t.createIndex({"a.b": 1});
    check(false);
});

MongoRunner.stopMongod(conn);
