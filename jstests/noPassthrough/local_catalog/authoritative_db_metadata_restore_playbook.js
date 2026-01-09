/*
 * Test that the playbook written for restoring a cluster to a consistent metadata state actually recovers it.
 *
 * Marked as 'requires_persistence' to prevent the test from running on the 'inMemory' variant,
 * because the restarted node relies on the replica set config persisted to disk to know that it
 * is initialized as part of a replica set and should run for election.
 * @tags: [
 *   requires_fcv_83,
 *   requires_persistence,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function getDbMetadataFromGlobalCatalog(db) {
    return db.getSiblingDB("config").databases.findOne({_id: db.getName()});
}

function validateShardCatalog(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.getDB("config").getCollection("shard.catalog.databases").findOne({_id: dbName});
    assert.eq(expectedDbMetadata, dbMetadataFromShard);
}

function overwriteGlobalCatalogConfigDbEntry(db, dbName, newValue) {
    assert.commandWorked(
        db
            .getSiblingDB("config")
            .databases.update({_id: dbName}, {$set: newValue}, {upsert: true, writeConcern: {w: "majority"}}),
    );
}

function overwriteShardCatalogConfigDbEntry(shardRs, dbName, newValue) {
    assert.commandWorked(
        shardRs
            .getPrimary()
            .getDB("config")
            .getCollection("shard.catalog.databases")
            .update({_id: dbName}, {$set: newValue}, {upsert: true, writeConcern: {w: "majority"}}),
    );
    const rsStatus = assert.commandWorked(shardRs.getPrimary().adminCommand({replSetGetStatus: 1}));
    const opTime = rsStatus.optimes.writtenOpTime.ts;
    jsTest.log.info({name: "Update opTime", opTime});
    shardRs.nodes.forEach((node) => {
        shardRs.waitForCheckpoint(node, opTime);
    });
    shardRs.stopSet(15, true);
    shardRs.startSet({}, true);
    shardRs.waitForPrimary();
    shardRs.awaitReplication();
}

function validateShardCatalogCache(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(dbMetadataFromShard);
    assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
}

function checkConsistency(db) {
    const res = db.checkMetadataConsistency();
    const inconsistencies = res.toArray();
    jsTest.log.info({name: "inconsistencies found", inconsistencies});
    assert.eq(0, inconsistencies.length);
}

function checkMoreRecentGlobalCatalogAndFixCSRS(st, db) {
    // Make the global catalog inconsistent with what we have on the shards by making it have a more recent version than all other shards.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    const originalVersionTimestamp = dbMetadataFromConfig.version.timestamp;
    dbMetadataFromConfig.version.timestamp = Timestamp(
        dbMetadataFromConfig.version.timestamp.getTime(),
        dbMetadataFromConfig.version.timestamp.getInc() + 1,
    );
    overwriteGlobalCatalogConfigDbEntry(db, db.getName(), dbMetadataFromConfig);
    assert.eq(getDbMetadataFromGlobalCatalog(db), dbMetadataFromConfig);

    // Restart all nodes so that the inconsistency is present throughout the system
    st.stopAllMongos();
    st.stopAllShards({}, true);
    st.stopAllConfigServers({}, true);
    st.restartAllConfigServers();
    st.restartAllShards();
    st.restartMongos(0);

    db = st.s.getDB(db.getName());
    // Artificially lower the inconsistency tassert down to a simple uassert, otherwise we will fail this test when we run checkMetadataConsistency.
    const failPoints = FixtureHelpers.mapOnEachShardNode({
        db: db.getSiblingDB("admin"),
        func: (db) => configureFailPoint(db, "avoidTassertForInconsistentMetadata"),
        primaryNodeOnly: false,
    });

    // At this point the cluster is in an inconsistent state, let's verify this with a
    // checkMetadataConsistency call.
    assert.throws(() => checkConsistency(db));

    // Restore the cluster to what it should have durably.
    dbMetadataFromConfig.version.timestamp = originalVersionTimestamp;
    overwriteGlobalCatalogConfigDbEntry(db, db.getName(), dbMetadataFromConfig);

    for (const fp of failPoints) {
        fp.off();
    }
}

function checkMoreRecentGlobalCatalogAndFixShards(st, db) {
    // Make the global catalog inconsistent with what we have on the shards by making it have a more recent version than all other shards.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    dbMetadataFromConfig.version.timestamp = Timestamp(
        dbMetadataFromConfig.version.timestamp.getTime(),
        dbMetadataFromConfig.version.timestamp.getInc() + 1,
    );
    overwriteGlobalCatalogConfigDbEntry(db, db.getName(), dbMetadataFromConfig);
    assert.eq(getDbMetadataFromGlobalCatalog(db), dbMetadataFromConfig);

    // Restart all nodes so that the inconsistency is present throughout the system
    st.stopAllMongos();
    st.stopAllShards({}, true);
    st.stopAllConfigServers({}, true);
    st.restartAllConfigServers();
    st.restartAllShards();
    st.restartMongos(0);

    db = st.s.getDB(db.getName());
    // Artificially lower the inconsistency tassert down to a simple uassert, otherwise we will fail this test when we run checkMetadataConsistency.
    const failPoints = FixtureHelpers.mapOnEachShardNode({
        db: db.getSiblingDB("admin"),
        func: (db) => configureFailPoint(db, "avoidTassertForInconsistentMetadata"),
        primaryNodeOnly: false,
    });

    // At this point the cluster is in an inconsistent state, let's verify this with a
    // checkMetadataConsistency call.
    assert.throws(() => checkConsistency(db));

    // Restore the cluster to what it should have durably.
    overwriteShardCatalogConfigDbEntry(st.rs0, db.getName(), dbMetadataFromConfig);

    for (const fp of failPoints) {
        fp.off();
    }
}

function checkConflictingPrimaryShards(st, db) {
    // Make the global catalog inconsistent with what we have on the shards by wrongly making another shard the primary.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    dbMetadataFromConfig.primary = st.shard1.shardName;
    overwriteGlobalCatalogConfigDbEntry(db, db.getName(), dbMetadataFromConfig);
    assert.eq(getDbMetadataFromGlobalCatalog(db), dbMetadataFromConfig);

    // Restart all nodes so that the inconsistency is present throughout the system.
    st.stopAllMongos();
    st.stopAllShards({}, true);
    st.stopAllConfigServers({}, true);
    st.restartAllConfigServers();
    st.restartAllShards();
    st.restartMongos(0);

    db = st.s.getDB(db.getName());

    // At this point the cluster is in an inconsistent state, let's verify this with a
    // checkMetadataConsistency call.
    assert.throws(() => checkConsistency(db));

    // Restore the durable metadata to what it should be.
    dbMetadataFromConfig.primary = st.shard0.shardName;
    overwriteGlobalCatalogConfigDbEntry(db, db.getName(), dbMetadataFromConfig);
}

const st = new ShardingTest({shards: 2, rs: {nodes: 3}, mongos: 1});

for (const fn of [
    checkConflictingPrimaryShards,
    checkMoreRecentGlobalCatalogAndFixCSRS,
    checkMoreRecentGlobalCatalogAndFixShards,
]) {
    jsTest.log.info(`Running test case: ${fn.name}`);

    const dbName = "test";
    let db = st.s.getDB(dbName);

    assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

    // Create a collection and have data present on it to check later.
    const testDoc = {x: 1};
    assert.commandWorked(db.testColl.insertOne(testDoc));

    st.awaitReplicationOnShards();

    // Validate that the db metadata in the shard catalog matches the global catalog.
    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(db);
    validateShardCatalog(db.getName(), st.rs0.getPrimary(), dbMetadataFromConfig);

    // Validate that the db metadata in the shard catalog cache matches the global catalog.
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(db.getName(), node, dbMetadataFromConfig);
    });

    // Verify the cluster is consistent on the database metadata before scrambling.
    assert.doesNotThrow(() => checkConsistency(db));

    // We now execute the scrambling. This function will check that we are inconsistent and restore the server to its correct durable state once we're done checking the inconsistency.
    fn(st, db);

    // Server now has the correct durable state again but may have the incorrect in-memory state. Run flushRouterConfig in order to reset the memory state.
    db = st.s.getDB(dbName);
    const cmd = {flushRouterConfig: db.getName()};
    st.configRS.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });
    st.rs0.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });
    st.rs1.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });
    st.forEachMongos((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });

    // At this point the cluster should now be in a consistent state with respect to the database.
    assert.doesNotThrow(() => checkConsistency(db));

    // Verify that we can still read the test document and insert a new one.
    assert.sameMembers(db.testColl.find({}, {_id: 0, x: 1}).toArray(), [testDoc]);
    assert.sameMembers(db.testColl.find({}, {_id: 0, x: 1}).readPref("secondary").toArray(), [testDoc]);
    assert.commandWorked(db.testColl.insertOne(testDoc));

    // Drop the database as cleanup for the next operation.
    db.dropDatabase();
}

st.stop();
