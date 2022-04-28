/**
 * Tests that time-series measurement indexes can be created in FCV 6.0.
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const collName = "coll";

const db = primary.getDB(dbName);

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));
assert.commandWorked(db.coll.insert({t: ISODate(), m: 1}));
assert.commandWorked(db.coll.createIndex({a: 1, t: 1}));

rst.stopSet();
}());
