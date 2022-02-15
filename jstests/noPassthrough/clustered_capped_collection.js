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

{
    const replSet = new ReplSetTest({name: "clustered_capped_collections", nodes: 1});
    replSet.startSet({setParameter: {ttlMonitorSleepSecs: 1}});
    replSet.initiate();

    const replicatedDB = replSet.getPrimary().getDB('replicated');
    const nonReplicatedDB = replSet.getPrimary().getDB('local');
    const collName = 'clustered_collection';
    const replicatedColl = replicatedDB[collName];
    const nonReplicatedColl = nonReplicatedDB[collName];

    replicatedColl.drop();
    nonReplicatedColl.drop();

    ClusteredCappedUtils.testClusteredCappedCollectionWithTTL(replicatedDB, collName, '_id');
    ClusteredCappedUtils.testClusteredTailableCursorCreation(
        replicatedDB, collName, '_id', true /* isReplicated */);
    for (let awaitData of [false, true]) {
        ClusteredCappedUtils.testClusteredTailableCursorWithTTL(
            replicatedDB, collName, '_id', true /* isReplicated */, awaitData);
        ClusteredCappedUtils.testClusteredTailableCursorCappedPositionLostWithTTL(
            replicatedDB, collName, '_id', true /* isReplicated */, awaitData);
        ClusteredCappedUtils.testClusteredTailableCursorOutOfOrderInsertion(
            replicatedDB, collName, '_id', true /* isReplicated */, awaitData);
    }
    ClusteredCappedUtils.testClusteredReplicatedTTLDeletion(replicatedDB, collName);

    replSet.stopSet();
}

// enableTestCommands is required for end users to create a capped clustered collection.
{
    TestData.enableTestCommands = false;
    const replSetNoTestCommands = new ReplSetTest({name: "clustered_capped_collections", nodes: 1});
    replSetNoTestCommands.startSet();
    replSetNoTestCommands.initiate();

    assert.commandFailedWithCode(
        replSetNoTestCommands.getPrimary().getDB("test").createCollection(
            'c',
            {clusteredIndex: {key: {_id: 1}, unique: true}, capped: true, expireAfterSeconds: 10}),
        6127800);

    replSetNoTestCommands.stopSet();
}
})();
