/**
 * Demonstrate that durable history can be used across a restart. The rough test outline:
 *
 * 1) Create a collection `existsAtOldestTs`. This collection and its `_id` index should be readable
 *    across a restart.
 * 2) Stop advancing the oldest timestamp with `WTPreserveSnapshotHistoryIndefinitely`.
 * 3) Create a secondary index on `existsAtOldestTs`. This index should not be readable across a
 *    restart. This is a conservative behavior that is perfectly reasonable to later relax.
 * 4) Create a collection `dneAtOldestTs`. This collection should not be readable across a restart.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

let replTest = new ReplSetTest({
    name: "use_history_after_restart",
    nodes: 1,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojson({storage: {recovery: 2}}),
            // Set the history window to zero to explicitly control the oldest timestamp. This is
            // necessary to predictably exercise the minimum visible timestamp initialization of
            // collections and indexes across a restart.
            minSnapshotHistoryWindowInSeconds: 0,
            // Disable the noop writer to avoid background writes that can unintentionally advance
            // the stable/oldest timestamps.
            writePeriodicNoops: false
        }
    }
});
let nodes = replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let testDB = primary.getDB("test");

// Create `existsAtOldestTs`. Insert one document with a majority write concern. The write concern
// guarantees the stable and oldest timestamp are bumped.
assert.commandWorked(testDB.createCollection("existsAtOldestTs"));
assert.commandWorked(testDB.runCommand(
    {insert: "existsAtOldestTs", documents: [{_id: 0, x: 0}], writeConcern: {w: "majority"}}));

// Preserving history stops advancing the oldest timestamp. All writes after this point must not be
// readable after the restart. Either due to a correct response being returned or the server
// responding with a `SnapshotUnavailable` error.
//
// Record the response. Treat the response's `opTime` as the `oldestTimestamp` that should be usable
// for a timestamped read after the restart.
let fpResp = assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely", mode: "alwaysOn"}));

// Insert a second document. This document will not be seen after the restart.
assert.commandWorked(testDB.runCommand({insert: "existsAtOldestTs", documents: [{x: 1}]}));

// Create an index that cannot be correctly used after a restart.
assert.commandWorked(testDB["existsAtOldestTs"].createIndex({x: 1}));

// Create a second collection that is not visible after a restart.
assert.commandWorked(testDB.createCollection("dneAtOldestTs"));

// Insert a document to the new collection with a majority write concern. This will bump the stable
// timestamp. Record the response and treat the response's `opTime` as the `stableTimestamp` that
// should be usable for a timestamped read after the restart.
let dneInsertResp = assert.commandWorked(testDB["dneAtOldestTs"].runCommand(
    {insert: "dneAtOldestTs", documents: [{}], writeConcern: {w: "majority"}}));

// Restart the node with the `WTPreserveSnapshotHistoryIndefinitely` set. This prevents startup from
// advancing the oldest timestamp. This allows us to reliably read at the existing `oldestTimestamp`
// after the restart.
replTest.restart(primary, {
    setParameter: {
        "failpoint.WTPreserveSnapshotHistoryIndefinitely": tojson({mode: "alwaysOn"}),
    }
});

primary = replTest.getPrimary();
let oldestTimestamp = fpResp["operationTime"];
let stableTimestamp = dneInsertResp["opTime"]["ts"];
jsTestLog({
    "Test Ops": primary.getDB("local")["oplog.rs"].find({ns: /test/}).sort({ts: -1}).toArray(),
    "OldestTimestamp (approximate)": oldestTimestamp,
    "StableTimestamp (approximate)": stableTimestamp
});

// We should successfully read the first insert from `existsAtOldestTs` when reading at the oldest
// timestamp. Querying for `_id: 0` will access both the `_id` index and the record
let result = primary.getDB("test").runCommand({
    find: "existsAtOldestTs",
    filter: {_id: 0},
    readConcern: {level: "snapshot", atClusterTime: oldestTimestamp}
});
jsTestLog({"ExistsRes": result});
assert.eq(1, result["cursor"]["firstBatch"].length);

// We should get an error that the newer secondary index is not available when reading at the oldest
// timestamp.
result = primary.getDB("test").runCommand({
    find: "existsAtOldestTs",
    hint: {x: 1},
    readConcern: {level: "snapshot", atClusterTime: oldestTimestamp}
});
jsTestLog({"Unavailable Index Result": result});
assert.commandFailedWithCode(result, ErrorCodes.BadValue);

// Reading from the index at the `stableTimestamp` should succeed and produce a correct result.
result = primary.getDB("test").runCommand({
    find: "existsAtOldestTs",
    hint: {x: 1},
    readConcern: {level: "snapshot", atClusterTime: stableTimestamp}
});
jsTestLog({"Available Index Result": result});
assert.eq(2, result["cursor"]["firstBatch"].length);

// Querying `dneAtOldestTs` at the oldest timestamp should fail with a `SnapshotUnavailable` error.
result = primary.getDB("test").runCommand(
    {find: "dneAtOldestTs", readConcern: {level: "snapshot", atClusterTime: oldestTimestamp}});
jsTestLog({"SnapshotUnavailable on dneAtOldestTs": result});

// The collection does not exist at this time so find will return an empty result set.
assert.commandWorked(result);
assert.eq(0, result["cursor"]["firstBatch"].length);

// Querying `dneAtOldestTs` at the stable timestamp should succeed with a correct result.
result = primary.getDB("test").runCommand(
    {find: "dneAtOldestTs", readConcern: {level: "snapshot", atClusterTime: stableTimestamp}});
jsTestLog({"Available dneAtOldestTs result": result});
assert.eq(1, result["cursor"]["firstBatch"].length);

replTest.stopSet();
})();
