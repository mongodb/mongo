// This is a regression test for post-image validation in the old update system, which is used when
// the featureCompatibilityVersion is 3.4.
(function() {
    "use strict";

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    const testDB = conn.getDB("test");

    // Test validation of elements added to an array that is represented in a "deserialized" format
    // in mutablebson. The added element is invalid because it is a DBRef with a missing $id.
    assert.writeOK(testDB.coll.insert({_id: 0, a: []}));
    assert.writeErrorWithCode(
        testDB.coll.update({_id: 0}, {$set: {"a.1": 0, "a.0": {$ref: "coll", $db: "test"}}}),
        ErrorCodes.InvalidDBRef);
    assert.docEq(testDB.coll.findOne({_id: 0}), {_id: 0, a: []});

    // Test validation of modified array elements that are accessed using a string that is
    // numerically equivalent to their fieldname. The modified element is invalid because it is a
    // DBRef with a missing $id.
    assert.writeOK(testDB.coll.insert({_id: 1, a: [0]}));
    assert.writeErrorWithCode(
        testDB.coll.update({_id: 1}, {$set: {"a.00": {$ref: "coll", $db: "test"}}}),
        ErrorCodes.InvalidDBRef);
    assert.docEq(testDB.coll.findOne({_id: 1}), {_id: 1, a: [0]});

    MongoRunner.stopMongod(conn);
}());
