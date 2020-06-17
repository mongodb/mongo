//
// Tests that refineCollectionShardKey can be safely aborted on step-up after a failover leads to
// only half of the internal transaction getting replicated. This is a regression test for
// SERVER-48527.
//
// Tag this test as 'requires_find_command' to prevent it from running in the legacy passthrough.
// @tags: [
//   requires_find_command,
//   requires_fcv_44,
//   exclude_from_large_txns
// ]
//

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/replsets/rslib.js");

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    useBridge: true,
    other: {
        configOptions: {
            setParameter: {
                // Ensure transactions have multiple oplog entries.
                maxNumberOfTransactionOperationsInSingleOplogEntry: 1,
                bgSyncOplogFetcherBatchSize: 1
            }
        }
    }
});
jsTestLog("Reconfig CSRS to have stable primary");
const csrs = st.configRS;
let cfg = csrs.getReplSetConfigFromNode(0);
cfg.settings.electionTimeoutMillis = csrs.kDefaultTimeoutMS;
cfg.settings.catchUpTimeoutMillis = 0;
cfg.settings.chainingAllowed = false;
reconfig(csrs, cfg, true);
waitForConfigReplication(csrs.getPrimary());
csrs.awaitReplication();

const kDbName = jsTestName();
const kCollName = 'foo';
const kNsName = kDbName + '.' + kCollName;

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
assert.commandWorked(st.s.getCollection(kNsName).createIndex({_id: 1, aKey: 1}));

let primary = csrs.getPrimary();
let secondaries = csrs.getSecondaries();
let newPrimary = secondaries[0];
st.s.disconnect(secondaries);

jsTest.log("Stop secondary oplog replication on the extra secondary so it will vote for anyone");
const stopReplProducerFailPoint = configureFailPoint(secondaries[1], 'stopReplProducer');

jsTest.log("Stop secondary oplog replication before the last operation in the transaction");
// The stopReplProducerOnDocument failpoint ensures that secondary stops replicating before
// applying the last operation in the transaction. This depends on the oplog fetcher batch size
// being 1. This also relies on the last operation in the transaction modifying 'config.chunks'.
const stopReplProducerOnDocumentFailPoint = configureFailPoint(
    newPrimary, "stopReplProducerOnDocument", {document: {"applyOps.ns": "config.chunks"}});

jsTestLog("Refining collection shard key in a parallel shell");
let parallelRefineFn = function(ns) {
    assert.commandWorked(db.adminCommand({refineCollectionShardKey: ns, key: {_id: 1, aKey: 1}}));
};
const awaitShell = startParallelShell(funWithArgs(parallelRefineFn, kNsName), st.s.port);

jsTestLog("Wait for the new primary to block on fail point");
stopReplProducerOnDocumentFailPoint.wait();

jsTestLog(`Triggering CSRS failover from ${primary.host} to ${newPrimary.host}`);
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
st.s.reconnect(newPrimary);

jsTestLog("Waiting for set to agree on the new primary, " + newPrimary.host);
csrs.awaitNodesAgreeOnPrimary();

jsTestLog("Wait for parallel shell to complete");
awaitShell();

// Make sure we won't apply the whole transaction by any chance.
jsTestLog("Wait for the new primary to stop replication after primary catch-up");
checkLog.contains(newPrimary, "Stopping replication producer");

jsTestLog("Enable replication on the new primary so that it can finish state transition");
stopReplProducerOnDocumentFailPoint.off();
assert.eq(csrs.getPrimary(), newPrimary);

jsTestLog("Re-enable replication on the extra secondary so it can catch up");
stopReplProducerFailPoint.off();
csrs.awaitReplication();

st.stop();
})();
