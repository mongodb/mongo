/*
 * Tests that if reconfig did not replicate before step up, the new primary
 * is not stale because its new election Id takes precedence over Set version when
 * comparing.
 *
 * @tags: [does_not_support_stepdowns, multiversion_incompatible]
 */
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/sharding/libs/sharded_index_util.js");

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const st = new ShardingTest({shards: {rs0: {nodes: [{}, {}, {rsConfig: {priority: 0}}]}}});
const rst = st.rs0;
const primary = rst.getPrimary();
if (primary !== nodes[0]) {
    st.stop();
    return;  // For simplicity.
}

const config = rst.getReplSetConfigFromNode();
jsTestLog(`Initial config ${tojson(config)}`);
config.version++;

const versionToBlock = config.version;
const termToBlock = config.term;

let pauseInReconfig = configureFailPoint(primary, "hangAfterReconfig");
let maxElectionIdSetVersionPairUpdated =
    configureFailPoint(primary, "maxElectionIdSetVersionPairUpdated");

// Fail points to prevent the new config from being replicated.
let testFpHangBeforeFetchingConfig1 = configureFailPoint(
    nodes[1], "skipBeforeFetchingConfig", {versionAndTerm: [versionToBlock, termToBlock]});
let testFpHangBeforeFetchingConfig2 = configureFailPoint(
    nodes[2], "skipBeforeFetchingConfig", {versionAndTerm: [versionToBlock, termToBlock]});
let runReconfigJoin = startParallelShell(
    funWithArgs(function(config) {
        jsTestLog(`Run reconfig`);
        let result = db.getSiblingDB("admin").runCommand({replSetReconfig: config});
        jsTestLog(`Reconfig finished, result ${tojson(result)}`);
    }, config), primary.port);
pauseInReconfig.wait();
maxElectionIdSetVersionPairUpdated.wait();

// After step up the new primary still has the Set at previous version.
const nextPrimary = rst.getSecondary();
jsTestLog("Step Up");
nextPrimary.adminCommand({replSetStepUp: 1});
pauseInReconfig.off();
runReconfigJoin();

testFpHangBeforeFetchingConfig1.off();
testFpHangBeforeFetchingConfig2.off();

// This command fails if mongos' RSM was stuck with stale primary unable to rollback Set version.
let res = ShardedIndexUtil.getPerShardIndexes(st.s.getCollection("config.system.sessions"));
jsTestLog(`Aggregate run on Mongos ${tojson(res)}`);

st.stop();
}());
