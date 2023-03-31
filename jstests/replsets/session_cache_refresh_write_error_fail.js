/**
 * Test that write errors resulting as part of refreshing logical session do not kill open cursors.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});

rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB("test");
const fp = configureFailPoint(db, "failAllUpdates");
const collection = db.getCollection("mycoll");
const sessionDb = db.getMongo().startSession().getDatabase(db.getName());
const sessionCollection = sessionDb.getCollection(collection.getName());

assert.commandWorked(sessionCollection.insert(Array.from({length: 5}, (_, i) => ({_id: i}))));

const res = assert.commandWorked(sessionCollection.runCommand("find", {batchSize: 2}));

assert.commandFailedWithCode(db.adminCommand({refreshLogicalSessionCacheNow: 1}),
                             ErrorCodes.InternalError);

assert.commandWorked(
    sessionDb.runCommand({getMore: res.cursor.id, collection: sessionCollection.getName()}));

fp.off();

rst.stopSet();
})();
