/**
 * Test that user deletes on capped collections are only allowed in FCV 5.0.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

assert.commandWorked(db.createCollection("a", {capped: true, size: 1024}));
assert.commandWorked(db.a.insert({_id: 1}));
assert.commandWorked(db.a.insert({_id: 2}));

// FCV 5.0.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(db.a.remove({_id: 1}));

// FCV 4.4.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.writeErrorWithCode(db.a.remove({_id: 2}), ErrorCodes.IllegalOperation);

MongoRunner.stopMongod(conn);
})();
