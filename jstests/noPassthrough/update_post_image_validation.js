// Verify that the update system correctly rejects invalid entries during post-image validation.
// @tags: [requires_fcv_50]
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const testDB = conn.getDB("test");

const isDotsAndDollarsEnabled = testDB.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                    .featureFlagDotsAndDollars.value;
if (!isDotsAndDollarsEnabled) {
    // Test validation of elements added to an array that is represented in a "deserialized" format
    // in mutablebson. The added element is invalid because it is a DBRef with a missing $id.
    assert.commandWorked(testDB.coll.insert({_id: 0, a: []}));
    assert.writeErrorWithCode(
        testDB.coll.update({_id: 0}, {$set: {"a.1": 0, "a.0": {$ref: "coll", $db: "test"}}}),
        ErrorCodes.InvalidDBRef);
    assert.docEq(testDB.coll.findOne({_id: 0}), {_id: 0, a: []});

    // Test validation of modified array elements that are accessed using a string that is
    // numerically equivalent to their fieldname. The modified element is invalid because it is a
    // DBRef with a missing $id.
    assert.commandWorked(testDB.coll.insert({_id: 1, a: [0]}));
    assert.writeErrorWithCode(
        testDB.coll.update({_id: 1}, {$set: {"a.00": {$ref: "coll", $db: "test"}}}),
        ErrorCodes.InvalidDBRef);
    assert.docEq(testDB.coll.findOne({_id: 1}), {_id: 1, a: [0]});
} else {
    // Test validation of elements added to an array that is represented in a "deserialized" format
    // in mutablebson. The added element is valid.
    assert.commandWorked(testDB.coll.insert({_id: 0, a: []}));
    assert.commandWorked(
        testDB.coll.update({_id: 0}, {$set: {"a.1": 0, "a.0": {$ref: "coll", $db: "test"}}}));
    assert.docEq(testDB.coll.findOne({_id: 0}), {_id: 0, a: [{$ref: "coll", $db: "test"}, 0]});

    // Test validation of modified array elements that are accessed using a string that is
    // numerically equivalent to their fieldname. The modified element is valid.
    assert.commandWorked(testDB.coll.insert({_id: 1, a: [0]}));
    assert.commandWorked(
        testDB.coll.update({_id: 1}, {$set: {"a.00": {$ref: "coll", $db: "test"}}}));
    assert.docEq(testDB.coll.findOne({_id: 1}), {_id: 1, a: [{$ref: "coll", $db: "test"}]});
}

MongoRunner.stopMongod(conn);
}());
