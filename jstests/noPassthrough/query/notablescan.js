// check notablescan mode
//
// @tags: [
//   # The test runs commands that are not allowed with security token: setParameter.
//   not_allowed_with_signed_security_token,
//   assumes_against_mongod_not_mongos,
//   # This test attempts to perform read operations after having enabled the notablescan server
//   # parameter. The former operations may be routed to a secondary in the replica set, whereas the
//   # latter must be routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_superuser_permissions,
//   does_not_support_stepdowns,
// ]

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {isIdhackOrExpress} from "jstests/libs/query/analyze_plan.js";

function checkError(err) {
    assert.includes(err.toString(), "'notablescan'");
}

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");
let db = conn.getDB(jsTestName());
const colName = jsTestName();
let coll = db.getCollection(colName);
coll.drop();

assert.commandWorked(db.adminCommand({setParameter: 1, notablescan: true}));

{
    if (0) {
        // TODO: SERVER-2222 This should actually throw an error as it performs a collection
        // scan.
        assert.throws(function() {
            coll.find({a: 1}).toArray();
        });
    }

    coll.insert({a: 1});
    let err = assert.throws(function() {
        coll.count({a: 1});
    });
    checkError(err);

    // TODO: SERVER-2222 This should actually throw an error as it performs a collection scan.
    assert.eq(1, coll.find({}).itcount());

    err = assert.throws(function() {
        coll.find({a: 1}).toArray();
    });
    checkError(err);

    err = assert.throws(function() {
        coll.find({a: 1}).hint({$natural: 1}).toArray();
    });
    assert.includes(err.toString(), "$natural");
    checkError(err);

    coll.createIndex({a: 1});
    assert.eq(0, coll.find({a: 1, b: 1}).itcount());
    assert.eq(1, coll.find({a: 1, b: null}).itcount());
}

{  // Run the testcase with a clustered index.
    assertDropAndRecreateCollection(db, colName, {clusteredIndex: {key: {_id: 1}, unique: true}});
    coll = db.getCollection(colName);
    assert.commandWorked(coll.insert({_id: 22}));
    assert.eq(1, coll.find({_id: 22}).itcount());
    let plan = coll.find({_id: 22}).explain();
    // Make sure the plan uses fast path
    assert(isIdhackOrExpress(db, plan));

    // Make sure the same works with an aggregate.
    assert.eq(1, coll.aggregate([{$match: {_id: 22}}]).itcount());
    plan = coll.explain().aggregate([{$match: {_id: 22}}]);
    // Make sure the plan uses Express
    assert(isIdhackOrExpress(db, plan));
    assert.commandWorked(
        db.runCommand({aggregate: colName, pipeline: [{$match: {_id: 22}}], cursor: {}}));
}

MongoRunner.stopMongod(conn);
