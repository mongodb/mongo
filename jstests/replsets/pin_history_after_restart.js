/**
 * This test uses the test only `pinHistoryReplicated` command to exercise the DurableHistoryPins
 * across restart.
 *
 * The `pinHistoryReplicated` command will pin the oldest timestamp at the requested time (with an
 * optional rounding up to oldest). If the pin is successfully, the pinned time is written to a
 * document inside `mdb_testing.pinned_timestamp`.
 *
 * When the `TestingDurableHistoryPin` is registered at startup, it will repin the oldest timestamp
 * at the minimum of all documents written to `mdb_testing.pinned_timestamp`.
 *
 * This test does the following:
 *
 * 1) Pin the timestamp with an artificially small value to take advantage of rounding.
 * 2) See that the timestamp is pinned via serverStatus.
 * 3) Restart the node, see the pin persist.
 * 4) Make a new pin at "original pin" + 1
 * 5) Remove the pin at "original pin"
 * 6) Restart the node, see the incremented pin value from serverStatus.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, requires_persistence]
 */

(function() {
"use strict";

function incTs(ts) {
    return Timestamp(ts.t, ts.i + 1);
}

let replTest = new ReplSetTest({
    name: "use_history_after_restart",
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to zero to more aggressively advance the oldest timestamp.
            minSnapshotHistoryWindowInSeconds: 0,
            logComponentVerbosity: tojson({storage: {recovery: 2}}),
        }
    }
});
let nodes = replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();

// Pin with an arbitrarily small timestamp. Let the rounding tell us where the pin ended up. The
// write to the `mdb_testing.pinned_timestamp` collection is not logged/replayed during replication
// recovery. Repinning across startup happens before replication recovery. Do a majority write for
// predictability of the test.
let result = assert.commandWorked(primary.adminCommand(
    {"pinHistoryReplicated": Timestamp(100, 1), round: true, writeConcern: {w: "majority"}}));
let origPinTs = result["pinTs"];
jsTestLog({"First pin result": result});

// Do some additional writes that would traditionally advance the oldest timestamp.
for (var idx = 0; idx < 10; ++idx) {
    assert.commandWorked(primary.getDB("test")["coll"].insert({}));
}
assert.commandWorked(primary.getDB("test")["coll"].insert({}, {writeConcern: {w: "majority"}}));

// Observe that the pinned timestamp matches the command response.
let serverStatus = assert.commandWorked(primary.adminCommand("serverStatus"));
let pinnedTs = serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];
assert.eq(origPinTs, pinnedTs);

// Restarting the primary should preserve the pin.
replTest.restart(primary);
primary = replTest.getPrimary();
serverStatus = assert.commandWorked(primary.adminCommand("serverStatus"));
pinnedTs = serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];
assert.eq(origPinTs, pinnedTs);

// Create a new pin at "ts + 1". This should succeed, but have no effect.
result = assert.commandWorked(
    primary.adminCommand({"pinHistoryReplicated": incTs(result["pinTs"]), round: false}));
jsTestLog({"Second pin result": result});
let newPinTs = result["pinTs"];
assert.eq(newPinTs, incTs(origPinTs));

// Remove the old pin at "ts".
assert.commandWorked(primary.getDB("mdb_testing")["pinned_timestamp"].remove(
    {"pinTs": origPinTs}, {writeConcern: {w: "majority"}}));

// Restarting the node should observe a pin at "ts + 1".
replTest.restart(primary);
primary = replTest.getPrimary();
serverStatus = assert.commandWorked(primary.adminCommand("serverStatus"));
pinnedTs = serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];
assert.eq(newPinTs, pinnedTs);
replTest.stopSet();
})();
