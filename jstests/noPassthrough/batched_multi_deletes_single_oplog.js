/**
 * Tests that batch deletes resulting with single operations create regular delete oplog entries.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

const dbName = jsTestName();
const collName = "test";
const collCount = 10;

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: x}))));
assert.eq(collCount, coll.countDocuments({}));

// Verify the delete will involve the BATCHED_DELETE stage.
const expl = db.runCommand({
    explain: {delete: coll.getName(), deletes: [{q: {_id: {$lte: 0}}, limit: 0}]},
    verbosity: "executionStats"
});
assert.commandWorked(expl);
assert.eq(expl["queryPlanner"]["winningPlan"]["stage"], "BATCHED_DELETE");

jsTestLog("Deleting all documents where _id <= 0");
assert.commandWorked(coll.deleteMany({_id: {$lte: 0}}));

let ops =
    rst.findOplog(primary, {op: 'c', ns: 'admin.$cmd', 'o.applyOps': {$exists: true}}).toArray();
assert.eq(0, ops.length, "Should not have an applyOps oplog entry: " + tojson(ops));

ops = rst.findOplog(primary, {op: 'd', ns: coll.getFullName(), o: {_id: 0}}).toArray();
assert.eq(1, ops.length, "Should have a delete oplog entry: " + tojson(ops));

rst.stopSet();
})();
