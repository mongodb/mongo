/**
 * This test uses the test only `pinHistoryReplicated` command to exercise DurableHistoryPins
 * across rollback.
 *
 * The `pinHistoryReplicated` command will pin the oldest timestamp at the requested time (with an
 * optional rounding up to oldest). If the pin is successfully, the pinned time is written to a
 * document inside `mdb_testing.pinned_timestamp`.
 *
 * For the purposes of this test, the write is timestamped as its replicated in the oplog. If the
 * write gets rolled back, this test ensures any pinning effect it had is removed.
 *
 * @tags: [requires_fcv_49, requires_majority_read_concern, requires_persistence]
 */
(function() {
"use strict";

load("jstests/replsets/libs/rollback_test.js");

let rst = new ReplSetTest({
    name: "history_rollback_test",
    nodes: 3,
    useBridge: true,
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({storage: {recovery: 2}})}}
});
rst.startSet();
const config = rst.getReplSetConfig();
config.members[2].priority = 0;
config.settings = {
    chainingAllowed: false
};
rst.initiateWithHighElectionTimeout(config);

let rollbackTest = new RollbackTest("history_rollback_test", rst);
let rollbackNode = rollbackTest.getPrimary();
rollbackTest.transitionToRollbackOperations();

let serverStatus = rollbackNode.adminCommand("serverStatus");
// When there is no pin, the `min pinned timestamp` value is `Timestamp::max()`. I don't believe
// there is a JS constant for `Timestamp::max()`, so we capture it now for later.
let maxTimestampValue =
    serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];

// Perform a write that pins history. This write will be rolled back.
let result = assert.commandWorked(
    rollbackNode.adminCommand({"pinHistoryReplicated": Timestamp(100, 1), round: true}));
let origPinTs = result["pinTs"];

serverStatus = rollbackNode.adminCommand("serverStatus");
let pinnedTs = serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];
assert.eq(origPinTs, pinnedTs);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

serverStatus = rollbackNode.adminCommand("serverStatus");
pinnedTs = serverStatus["wiredTiger"]["snapshot-window-settings"]["min pinned timestamp"];
assert.eq(maxTimestampValue, pinnedTs);

rst.stopSet();
})();
