/**
 * Validate clustered capped collections.
 *
 * @tags: [
 *   requires_fcv_53,
 *   requires_replication,
 *   does_not_support_stepdowns,
 *   incompatible_with_eft
 * ]
 */
(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/clustered_collections/clustered_capped_utils.js");

const replSet = new ReplSetTest({name: "clustered_capped_collections", nodes: 1});
replSet.startSet({setParameter: {ttlMonitorSleepSecs: 1, supportArbitraryClusterKeyIndex: true}});
replSet.initiate();

const replicatedDB = replSet.getPrimary().getDB('replicated');
const nonReplicatedDB = replSet.getPrimary().getDB('local');
const collName = 'clustered_collection';
const replicatedColl = replicatedDB[collName];
const nonReplicatedColl = nonReplicatedDB[collName];

replicatedColl.drop();
nonReplicatedColl.drop();

ClusteredCappedUtils.testClusteredCappedCollectionWithTTL(nonReplicatedDB, collName, 'ts');
ClusteredCappedUtils.testClusteredTailableCursorCreation(
    nonReplicatedDB, collName, 'ts', false /* isReplicated */);
for (let awaitData of [false, true]) {
    ClusteredCappedUtils.testClusteredTailableCursorWithTTL(
        nonReplicatedDB, collName, 'ts', false /* isReplicated */, awaitData);
    ClusteredCappedUtils.testClusteredTailableCursorCappedPositionLostWithTTL(
        nonReplicatedDB, collName, 'ts', false /* isReplicated */, awaitData);
    ClusteredCappedUtils.testClusteredTailableCursorOutOfOrderInsertion(
        nonReplicatedDB, collName, 'ts', false /* isReplicated */, awaitData);
}

replSet.stopSet();
})();
