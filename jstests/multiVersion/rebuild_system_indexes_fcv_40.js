/**
 * Ensures that it is possible to rebuild a missing system index in FCV 4.0. See SERVER-43338.
 */
(function() {
"use strict";

const dbpath = MongoRunner.dataPath + "rebuild_system_indexes";
let conn = MongoRunner.runMongod({dbpath});
assert.neq(null, conn);

let db = conn.getDB('admin');

// Create the admin.system.users collection and its indexes.
assert.commandWorked(db.runCommand({createUser: 'test', pwd: 'test', roles: []}));

// This collection should have 2 indexes, including the _id index.
let res = db.system.users.runCommand("listIndexes");
assert.eq(2, res.cursor.firstBatch.length);

// Downgrade the FCV.
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

// Drop the secondary index on this collection, which will need to be rebuilt on startup.
assert.commandWorked(db.system.users.dropIndexes());
MongoRunner.stopMongod(conn);

// Restart the server so it will rebuild the system index.
conn = MongoRunner.runMongod({dbpath, noCleanData: true});
assert.neq(null, conn);

// Ensure the index is rebuilt.
db = conn.getDB('admin');
res = db.system.users.runCommand("listIndexes");
assert.eq(2, res.cursor.firstBatch.length);

MongoRunner.stopMongod(conn);
})();
