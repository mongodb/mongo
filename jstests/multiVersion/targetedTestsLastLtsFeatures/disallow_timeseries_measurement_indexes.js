/**
 * Tests that users cannot create time-series measurement indexes when FCV is less than 6.0.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");

const newVersion = "latest";
const oldVersion = "last-lts";
const nodes = {
    n1: {binVersion: newVersion},
    n2: {binVersion: oldVersion, rsConfig: {priority: 0}}
};

const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

let primary = rst.getPrimary();
let db = primary.getDB(dbName);
let coll = db.getCollection(collName);

// Create a time-series collection.
const timeField = "time";
const metaField = "meta";
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

// Cannot create a time-series measurement index.
assert.commandFailedWithCode(coll.createIndex({a: 1}), ErrorCodes.CannotCreateIndex);

rst.awaitReplication();
rst.stopSet();
}());
