// Test that arrayFilters usage is restricted when the featureCompatibilityVersion is 3.4.

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    let testDB = conn.getDB("arrayFilters_feature_compatibility_version");
    assert.commandWorked(testDB.dropDatabase());
    let coll = testDB.coll;

    let adminDB = conn.getDB("admin");

    let res;

    //
    // arrayFilters is not permitted when the featureCompatibilityVersion is 3.4.
    //

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Update.
    res = coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
    assert.writeError(res, ErrorCodes.InvalidOptions);
    assert.neq(
        -1,
        res.getWriteError().errmsg.indexOf(
            "The featureCompatibilityVersion must be 3.6 to use arrayFilters. See http://dochub.mongodb.org/core/3.6-feature-compatibility."),
        "update failed for a reason other than featureCompatibilityVersion");

    // FindAndModify.
    assert.throws(function() {
        coll.findAndModify(
            {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]});
    });

    // Update explain.
    assert.throws(function() {
        coll.explain().update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]});
    });

    // FindAndModify explain.
    assert.throws(function() {
        coll.explain().findAndModify(
            {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]});
    });

    //
    // arrayFilters is permitted when the featureCompatibilityVersion is 3.6.
    //

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Update.
    assert.writeOK(coll.update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]}));

    // FindAndModify.
    assert.eq(null,
              coll.findAndModify(
                  {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}));

    // Update explain.
    assert.commandWorked(
        coll.explain().update({_id: 0}, {$set: {"a.$[i]": 5}}, {arrayFilters: [{i: 0}]}));

    // FindAndModify explain.
    assert.commandWorked(coll.explain().findAndModify(
        {query: {_id: 0}, update: {$set: {"a.$[i]": 5}}, arrayFilters: [{i: 0}]}));

    MongoRunner.stopMongod(conn);
}());
