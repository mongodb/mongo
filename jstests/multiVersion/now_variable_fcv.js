/**
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod({binVersion: "latest"});
const db = conn.getDB(jsTest.name());

const coll = db[jsTest.name()];
const view42 = "viewWithNow42";
coll.drop();
assert.commandWorkedOrFailedWithCode(db.runCommand({drop: view42}), ErrorCodes.NamespaceNotFound);

// Just insert a single document so we have something to work with.
assert.writeOK(coll.insert({a: 1}));

assert.commandWorked(db.createView(view42, coll.getName(), [{$addFields: {timeField: "$$NOW"}}]),
                     'Expected a view with $$NOW to succeed');

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

// It should not be possble to create a view with $$NOW in the 4.0 mode.
assert.commandFailedWithCode(
    db.createView("viewWithNow", coll.getName(), [{$addFields: {timeField: "$$NOW"}}]),
    ErrorCodes.QueryFeatureNotAllowed,
    'Expected a view with $$NOW to fail');

// It should not be possble to create a view with $$CLUSTER_TIME in the 4.0 mode.
assert.commandFailedWithCode(
    db.createView("viewWithNow", coll.getName(), [{$addFields: {timeField: "$$CLUSTER_TIME"}}]),
    ErrorCodes.QueryFeatureNotAllowed,
    'Expected a view with $$CLUSTER_TIME to fail');

// But querying the existing views continue to work.
assert.commandWorked(db.runCommand({aggregate: view42, pipeline: [], cursor: {}}),
                     'Expected an aggregate with view to work');

MongoRunner.stopMongod(conn);
}());
