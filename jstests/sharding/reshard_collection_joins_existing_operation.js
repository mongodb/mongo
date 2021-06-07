/**
 * Tests that if a _configsvrReshardCollection command is issued and then the same command gets
 * issued again, the second command issued joins with the instance of resharding already in
 * progress.
 *
 * Use _configsvrReshardCollection instead of reshardCollection to exercise the behavior of the
 * config server in the absence of the distributed lock taken by _shardsvrReshardCollection on the
 * primary shard for the database.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/libs/parallelTester.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

// Generates a new thread to run _configsvrReshardCollection.
const makeConfigsvrReshardCollectionThread = (mongosConnString, configsvrReshardCollectionCmd) => {
    return new Thread((mongosConnString, configsvrReshardCollectionCmd) => {
        load("jstests/libs/discover_topology.js");
        const mongos = new Mongo(mongosConnString);
        const topology = DiscoverTopology.findConnectedNodes(mongos);
        const configsvr = new Mongo(topology.configsvr.nodes[0]);
        assert.commandWorked(configsvr.adminCommand(configsvrReshardCollectionCmd));
    }, mongosConnString, configsvrReshardCollectionCmd);
};

const constructTempReshardingNs = (dbName, collName) => {
    const sourceCollectionUUID = getUUIDFromListCollections(dbName, collName);
    const sourceCollectionUUIDString = extractUUIDFromObject(sourceCollectionUUID);
    return `${dbName}.system.resharding.${sourceCollectionUUIDString}`;
};

// Callers must ensure the temporary collection has actually been created by the time this is
// called.
const getTempUUID = (tempNs) => {
    const tempCollection = mongos.getCollection(tempNs);
    return getUUIDFromConfigCollections(mongos, tempCollection.getFullName());
};

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 2},
});

const sourceCollection = st.s.getCollection("reshardingDb.coll");

CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {oldKey: 1}, [
    {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: st.shard0.shardName},
]);

const reshardKey = {
    newKey: 1
};
const configsvrReshardCollectionCmd = {
    _configsvrReshardCollection: sourceCollection.getFullName(),
    key: reshardKey,
    writeConcern: {w: "majority"}
};

// Before starting the actual resharding, get the source collection's UUID to construct the
// namespace for the temporary collection that will be created.
const sourceDBName = sourceCollection.getDB();
const sourceCollName = sourceCollection.getName();
const tempNs = constructTempReshardingNs(sourceDBName, sourceCollName);

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const reshardCollectionThread1 =
    makeConfigsvrReshardCollectionThread(topology.mongos.nodes[0], configsvrReshardCollectionCmd);
const pauseBeforeCloningFP =
    configureFailPoint(configsvr, "reshardingPauseCoordinatorBeforeCloning");

// Issue the first _configsvrReshardCollection command and pause after the temporary collection is
// created but before its config.collections entry is replaced.
reshardCollectionThread1.start();
pauseBeforeCloningFP.wait();

// The UUID of the temporary resharding collection should become the UUID of the original collection
// once resharding has completed.
const expectedUUIDAfterReshardingCompletes = getTempUUID(tempNs);

const reshardCollectionJoinedFP =
    configureFailPoint(configsvr, "reshardCollectionJoinedExistingOperation");
const reshardCollectionThread2 =
    makeConfigsvrReshardCollectionThread(topology.mongos.nodes[0], configsvrReshardCollectionCmd);

// Hitting the reshardCollectionJoinedFP is additional confirmation that the second
// _configsvrReshardCollection command (identical to the first) gets joined with the instance
// created/running for the first command issued.
reshardCollectionThread2.start();
reshardCollectionJoinedFP.wait();

reshardCollectionJoinedFP.off();
pauseBeforeCloningFP.off();

reshardCollectionThread2.join();
reshardCollectionThread1.join();

// Confirm the UUID for the namespace that was resharded is the same as the temporary collection's
// UUID before the second reshardCollection command was issued.
const finalSourceCollectionUUID = getUUIDFromListCollections(sourceDBName, sourceCollName);
assert.eq(expectedUUIDAfterReshardingCompletes, finalSourceCollectionUUID);

st.stop();
})();
