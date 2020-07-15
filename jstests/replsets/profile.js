// Confirm that implicitly created profile collections are successful and do not trigger assertions.
// In order to implicitly create a profile collection with a read, we must set up the server with
// some data to read without the profiler being active.
// @tags: [requires_persistence]
(function() {
"use strict";
let rst = new ReplSetTest({nodes: {n0: {profile: "0"}}});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();
let primaryDB = primary.getDB('test');

assert.commandWorked(primaryDB.foo.insert({_id: 1}));

const nodeId = rst.getNodeId(primary);
rst.stop(nodeId);
rst.start(nodeId, {profile: "2"}, true /* preserves data directory */);
rst.awaitReplication();
primary = rst.getPrimary();
primaryDB = primary.getDB('test');

let oldAssertCounts = primaryDB.serverStatus().asserts;
assert.eq(0, primaryDB.system.profile.count());
assert.eq([{_id: 1}], primaryDB.foo.aggregate([]).toArray());
let newAssertCounts = primaryDB.serverStatus().asserts;
assert.eq(oldAssertCounts, newAssertCounts);
assert.eq(1, primaryDB.system.profile.count());

rst.stopSet();
})();
