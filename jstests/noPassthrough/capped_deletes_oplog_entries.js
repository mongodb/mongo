/**
 * Tests that capped deletes generate oplog entries in FCV >= 5.0 only.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const collName = "capped";

const db = primary.getDB(dbName);
assert.commandWorked(db.createCollection(collName, {capped: true, size: 100, max: 1}));

const coll = db.getCollection(collName);

// In FCV <= 4.4, capped deletes do not generate oplog entries.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));

const oplog = primary.getDB("local").getCollection("oplog.rs");
assert.eq(2, oplog.find({op: "i", ns: coll.getFullName()}).count());
assert.eq(0, oplog.find({op: "d", ns: coll.getFullName()}).count());
assert.eq(1, coll.count());

// In FCV >= 5.0, capped deletes generate oplog entries.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
assert.commandWorked(coll.insert([{a: 3}, {a: 4}]));

assert.eq(4, oplog.find({op: "i", ns: coll.getFullName()}).count());
assert.eq(2, oplog.find({op: "d", ns: coll.getFullName()}).count());
assert.eq(1, coll.count());

rst.stopSet();
}());
