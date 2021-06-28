/**
 * Tests the behavior of how getMore operations are routed by the mongo shell when using a replica
 * set connection and cursors are established on a secondary.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";
var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "getmore";

// We create our own replica set connection because 'rst.nodes' is an array of direct
// connections to each individual node.
var conn = new Mongo(rst.getURL());

var coll = conn.getDB(dbName)[collName];
coll.drop();

// Insert several document so that we can use a cursor to fetch them in multiple batches.
var res = coll.insert([{}, {}, {}, {}, {}]);
assert.commandWorked(res);
assert.eq(5, res.nInserted);

// Wait for the secondary to catch up because we're going to try and do reads from it.
rst.awaitReplication();

// Establish a cursor on the secondary and verify that the getMore operations are routed to it.
var cursor = coll.find().readPref("secondary").batchSize(2);
assert.eq(5, cursor.itcount(), "failed to read the documents from the secondary");

rst.stopSet();
})();
