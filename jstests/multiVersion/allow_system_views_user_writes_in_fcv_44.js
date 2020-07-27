/**
 * Tests that users are allowed to write to the system.views collecion if not on the latest FCV.
 *
 * TODO (SERVER-49545): Remove this test when 5.0 becomes last-lts.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(conn, null);
const db = conn.getDB("test");

const viewNs = "test.view";
const viewDefinition = {
    _id: viewNs,
    viewOn: "coll",
    pipeline: []
};
const invalidField = {
    invalidField: true
};

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

db.system.views.drop();
assert.commandWorked(db.createCollection("system.views"));

assert.commandWorked(db.system.views.insert(viewDefinition));
assert.commandWorked(db.system.views.update({}, invalidField));
assert.commandWorked(db.system.views.remove({}));

MongoRunner.stopMongod(conn);
})();
