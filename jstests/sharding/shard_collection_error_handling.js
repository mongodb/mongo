/*
 * Test to validate the correct behaviour of shardCollection command when finding errors and it is
 * needed to rollback or continue the operation.
 *
 * @tags: [
 *    featureFlagAuthoritativeShardCollection,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

// Configure initial sharding cluster
const st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

const dbName = jsTestName();

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

function testNonRetriableErrorInsideCommitPhase(createAsUnsharded) {
    const collName = "collA_" + (createAsUnsharded ? "createAsUnsharded" : "implicitCreate");
    const ns = dbName + "." + collName;

    jsTestLog("Testing non retriable error inside the commit phase for " + ns);

    if (createAsUnsharded) {
        st.s.getDB(dbName).createCollection(collName);
    }

    let fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeCommitOnShardingCatalog");

    // Start creating a new sharded collection in a parallel shell and hang before committing.
    const awaitShardCollection = startParallelShell(
        funWithArgs(function(ns) {
            assert.commandFailedWithCode(db.adminCommand({shardCollection: ns, key: {x: "hashed"}}),
                                         ErrorCodes.InvalidOptions);
        }, ns), st.s.port);

    fp.wait();

    // Add a zone associated to the shard with an invalid shard key regarding the last
    // shardCollection request. This addZone is concurrent with the shardCollection, and it will be
    // effective before committing the new collection to the sharding catalog.
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "A_1"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {y: 0}, max: {y: 10}, zone: "A_1"}));

    // Force a stepdown to make the coordinator being re-executed and calculate again all non
    // persisted variables, i.e. chunk distribution.
    const primaryNode = st.rs0.getPrimary();
    st.rs0.freeze(primaryNode);
    st.rs0.waitForPrimary();

    awaitShardCollection();
    fp.off();
    st.rs0.unfreeze(primaryNode);

    // Validate that previous run of the shardCollection command has not left the cluster in an
    // inconsistent state and we are able to create the collection successfully.
    const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));

    // Validate that there is no local collection on the db primary shard in case of implicit
    // shardCollection create, otherwise it must exist.
    // TODO SERVER-83774: Create collection coordinator should clean up the collection on the db
    // primary shard in case of rollback.
    let rs0Collections = assert.commandWorked(st.rs0.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs0Collections.cursor.firstBatch.length);

    // Validate that there is no local collection on the participant shard.
    let rs1Collections = assert.commandWorked(st.rs1.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(0, rs1Collections.cursor.firstBatch.length);

    if (!createAsUnsharded) {
        // Use retryWrites when writing to the configsvr because mongos does not automatically retry
        // those.
        const mongosSession = st.s.startSession({retryWrites: true});
        const configDB = mongosSession.getDatabase("config");
        const collEntry = configDB.collections.findOne({_id: ns});
        assert.eq(undefined, collEntry);
    }

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {y: 1}}));
}

testNonRetriableErrorInsideCommitPhase(true /* createAsUnsharded */);
testNonRetriableErrorInsideCommitPhase(false /* createAsUnsharded */);

function testRetriableErrorWithoutInvolvingDBPrimaryShardAtSecondExecution(createAsUnsharded) {
    const collName = "collB_" + (createAsUnsharded ? "createAsUnsharded" : "implicitCreate");
    const ns = dbName + "." + collName;

    jsTestLog(
        "Testing retriable error without involving the db primary shard at second execution for " +
        ns);

    if (createAsUnsharded) {
        st.s.getDB(dbName).createCollection(collName);
    }

    let fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeCommitOnShardingCatalog");

    // Add a zone associated to each shard.
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "A_2"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: 0}, zone: "A_2"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "B_2"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 0}, max: {x: MaxKey}, zone: "B_2"}));

    // Start creating a new sharded collection in a parallel shell and hang before committing.
    const awaitShardCollection = startParallelShell(
        funWithArgs(function(ns) {
            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
        }, ns), st.s.port);

    fp.wait();

    // Remove the zone associated to the db primary shard, so on second execution it will not
    // receive any chunk.
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "A_2"}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard0.shardName, zone: "A_2"}));

    // Force a stepdown to make the coordinator being re-executed and calculate again all non
    // persisted variables, i.e. chunk distribution.
    const primaryNode = st.rs0.getPrimary();
    st.rs0.freeze(primaryNode);
    st.rs0.waitForPrimary();

    awaitShardCollection();
    fp.off();
    st.rs0.unfreeze(primaryNode);

    // Validate that the collection exists on the db primary shard.
    let rs0Collections = assert.commandWorked(st.rs0.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs0Collections.cursor.firstBatch.length);

    // Validate that the collection exists on shard1.
    let rs1Collections = assert.commandWorked(st.rs1.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs1Collections.cursor.firstBatch.length);

    // Validate that shard1 owns two chunks, that is, one for each zone.
    const mongosSession = st.s.startSession({retryWrites: true});
    const configDB = mongosSession.getDatabase("config");
    const collUUID = configDB.collections.findOne({_id: ns}).uuid;
    const chunks = configDB.chunks.find({uuid: collUUID}).toArray();
    assert.eq(2, chunks.length);
    assert.eq(st.shard1.shardName, chunks[0].shard);
    assert.eq(st.shard1.shardName, chunks[1].shard);
}

testRetriableErrorWithoutInvolvingDBPrimaryShardAtSecondExecution(true /* createAsUnsharded */);
testRetriableErrorWithoutInvolvingDBPrimaryShardAtSecondExecution(false /* createAsUnsharded */);

function testRetriableErrorWithoutInvolvingParticipantShardAtSecondExecution(createAsUnsharded) {
    const collName = "collC_" + (createAsUnsharded ? "createAsUnsharded" : "implicitCreate");
    const ns = dbName + "." + collName;

    jsTestLog(
        "Testing retriable error without involving participant shards at second execution for " +
        ns);

    if (createAsUnsharded) {
        st.s.getDB(dbName).createCollection(collName);
    }

    let fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeCommitOnShardingCatalog");

    // Add a zone associated to each shard.
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "A_3"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: 0}, zone: "A_3"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "B_3"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {x: 0}, max: {x: MaxKey}, zone: "B_3"}));

    // Start creating a new sharded collection in a parallel shell and hang before committing.
    const awaitShardCollection = startParallelShell(
        funWithArgs(function(ns) {
            assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
        }, ns), st.s.port);

    fp.wait();

    // Remove the zone associated to the db primary shard, so on second execution it will not
    // receive any chunk.
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "B_3"}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: st.shard1.shardName, zone: "B_3"}));

    // Force a stepdown to make the coordinator being re-executed and calculate again all non
    // persisted variables, i.e. chunk distribution.
    const primaryNode = st.rs0.getPrimary();
    st.rs0.freeze(primaryNode);
    st.rs0.waitForPrimary();

    awaitShardCollection();
    fp.off();
    st.rs0.unfreeze(primaryNode);

    // Validate that the collection exists on the db primary shard.
    let rs0Collections = assert.commandWorked(st.rs0.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs0Collections.cursor.firstBatch.length);

    // Validate that the collection does not exist on shard1.
    let rs1Collections = assert.commandWorked(st.rs1.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(0, rs1Collections.cursor.firstBatch.length);

    // Validate that shard0 owns two chunks, that is, one for each zone.
    const mongosSession = st.s.startSession({retryWrites: true});
    const configDB = mongosSession.getDatabase("config");
    const collUUID = configDB.collections.findOne({_id: ns}).uuid;
    const chunks = configDB.chunks.find({uuid: collUUID}).toArray();
    assert.eq(2, chunks.length);
    assert.eq(st.shard0.shardName, chunks[0].shard);
    assert.eq(st.shard0.shardName, chunks[1].shard);
}

testRetriableErrorWithoutInvolvingParticipantShardAtSecondExecution(true /* createAsUnsharded */);
testRetriableErrorWithoutInvolvingParticipantShardAtSecondExecution(false /* createAsUnsharded */);

(function testShardCollectionDroppingByUUIDAtRollback() {
    const collName = "collD";
    const ns = dbName + "." + collName;

    // By a direct connection, create a misplaced collection.
    assert.commandWorked(st.shard1.getCollection(ns).insert({x: "foo"}));

    // Validate that shardCollection will fail when trying to create the participant collection.
    assert.commandFailedWithCode(st.s.adminCommand({shardCollection: ns, key: {x: "hashed"}}),
                                 ErrorCodes.InvalidUUID);

    // Validate that the collection still exists on shard1 because the shardCollection rollback has
    // not drop it.
    const rs1Collections = assert.commandWorked(st.rs1.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs1Collections.cursor.firstBatch.length);

    // Manually drop the collection to pass the metadata inconsistency hook.
    assert(st.shard1.getCollection(ns).drop());
})();

(function testShardCollectionOutsideDbPrimaryWithoutInvolvingDataShard() {
    const collName = "collE";
    const ns = dbName + "." + collName;

    jsTestLog(
        "Testing shard collection living outside dbPrimary without chunks on the data shard for " +
        ns);

    // Create an unsplittable collection living outside the dbPrimary
    assert.commandWorked(st.s.getDB(dbName).runCommand(
        {createUnsplittableCollection: collName, dataShard: st.shard1.shardName}));

    // Create zones that will force the entire collection onto shard 0 (dbPrimary)
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "E_1"}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: MaxKey}, zone: "E_1"}));

    // Shard the collection
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Ensure that the collection only exists on the dbPrimary
    const rs0Collections = assert.commandWorked(st.rs0.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(1, rs0Collections.cursor.firstBatch.length);
    const rs1Collections = assert.commandWorked(st.rs1.getPrimary().getDB(dbName).runCommand(
        {listCollections: 1, filter: {name: collName}}));
    assert.eq(0, rs1Collections.cursor.firstBatch.length);
})();

st.stop();
