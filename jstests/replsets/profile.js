// Confirm that implicitly created profile collections are successful and do not trigger assertions.
// In order to implicitly create a profile collection with a read, we must set up the server with
// some data to read without the profiler being active.
// @tags: [
//   requires_fcv_81,
//   requires_persistence,
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({nodes: {n0: {profile: "0"}}});
rst.startSet({setParameter: {buildInfoAuthMode: "allowedPreAuth"}});
rst.initiate();
let primary = rst.getPrimary();
let primaryDB = primary.getDB("test");

primaryDB.setLogLevel(5, "assert");

assert.commandWorked(primaryDB.foo.insert({_id: 1}));

const nodeId = rst.getNodeId(primary);
rst.stop(nodeId);
rst.start(nodeId, {profile: "2"}, true /* preserves data directory */);
rst.awaitReplication();
primary = rst.getPrimary();
primaryDB = primary.getDB("test");

primaryDB.setLogLevel(5, "assert");

let oldAssertCounts = primaryDB.serverStatus().asserts;
jsTestLog("Before running aggregation: Assert counts reported by db.serverStatus(): " + tojson(oldAssertCounts));
try {
    assert.eq(0, primaryDB.system.profile.count());
    assert.eq([{_id: 1}], primaryDB.foo.aggregate([]).toArray());

    let newAssertCounts = primaryDB.serverStatus().asserts;
    jsTestLog("After running aggregation: Assert counts reported by db.serverStatus(): " + tojson(newAssertCounts));
    assert.eq(oldAssertCounts, newAssertCounts);
    // Should have 2 entries, one for the count command and one for the aggregate command.
    assert.eq(2, primaryDB.system.profile.count());
} finally {
    primaryDB.setLogLevel(0, "assert");
}

rst.stopSet();
