/*
 * Test for validating the correct behaviour of the CloneAuthoritativeMetadata DDL. This test aims
 * to check that the database metadata is cloned from the global catalog to the shard catalog
 * and cache correctly.
 *
 * @tags: [
 *     featureFlagAuthoritativeShardsCRUD,
 *     featureFlagAuthoritativeShardsDDL,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

function getDbMetadataFromGlobalCatalog(db) {
    return db.getSiblingDB("config").databases.findOne({_id: db.getName()});
}

function validateShardCatalog(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard
        .getDB("config")
        .shard.catalog.databases.findOne({_id: dbName});
    assert.eq(expectedDbMetadata, dbMetadataFromShard);
}

function validateShardCatalogCache(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(dbMetadataFromShard);

    if (expectedDbMetadata) {
        assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
        assert.eq(true, dbMetadataFromShard.isPrimaryShardForDb, dbMetadataFromShard);
    } else {
        // A shard that does not own the database must not report itself as its primary shard.
        // It may still retain legacy non-authoritative DSR metadata naming another shard as
        // primary while transitioning to the authoritative model, so do not assert that the DSR is
        // empty.
        assert(!dbMetadataFromShard.isPrimaryShardForDb, dbMetadataFromShard);
    }
}

function runCloningDDL(conn) {
    assert.commandWorked(
        conn.adminCommand({
            _shardsvrCloneAuthoritativeMetadata: 1,
            databaseVersion: {
                uuid: new UUID(),
                timestamp: new Timestamp(1, 0),
                lastMod: NumberInt(1),
            },
            writeConcern: {w: "majority"},
        }),
    );
}

// Stops a FCV upgrade in the transitional kUpgrading FCV, before the authoritative metadata cloning.
function enterTransitionalUpgradingFCV() {
    configureFailPoint(st.configRS.getPrimary(), "failUpgrading", {}, {times: 1});
    assert.commandFailedWithCode(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        549180,
    );
}

{
    jsTest.log("Validate how the cloning DDL clones all databases from the global catalog");

    const fooDbName = "foo";
    const barDbName = "bar";

    const fooDb = st.s.getDB(fooDbName);
    const barDb = st.s.getDB(barDbName);

    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    assert.commandWorked(
        st.s.adminCommand({enableSharding: fooDbName, primaryShard: st.shard1.shardName}),
    );
    assert.commandWorked(
        st.s.adminCommand({enableSharding: barDbName, primaryShard: st.shard1.shardName}),
    );
    enterTransitionalUpgradingFCV();

    runCloningDDL(st.shard1);

    // Make sure the database metadata is installed on all nodes.
    st.awaitReplicationOnShards();

    // Validate that db metadata in the shard catalog and cache matches the global catalog.
    let dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(fooDb);
    validateShardCatalog(fooDbName, st.shard1, dbMetadataFromConfig);
    st.rs1.nodes.forEach((node) => {
        validateShardCatalogCache(fooDbName, node, dbMetadataFromConfig);
    });

    dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(barDb);
    validateShardCatalog(barDbName, st.shard1, dbMetadataFromConfig);
    st.rs1.nodes.forEach((node) => {
        validateShardCatalogCache(barDbName, node, dbMetadataFromConfig);
    });

    assert.commandWorked(fooDb.dropDatabase());
    assert.commandWorked(barDb.dropDatabase());
}

{
    jsTest.log("Validate idempotency of the cloning DDL");

    const dbName = "idempotentDb";

    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
    enterTransitionalUpgradingFCV();

    runCloningDDL(st.shard0);
    runCloningDDL(st.shard0);

    st.awaitReplicationOnShards();

    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(st.s.getDB(dbName));
    validateShardCatalog(dbName, st.shard0, dbMetadataFromConfig);
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(dbName, node, dbMetadataFromConfig);
    });

    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
}

function validateCloningDDLWithConcurrentOperation(dbName, op) {
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
    enterTransitionalUpgradingFCV();

    const fp = configureFailPoint(
        st.shard0,
        "hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL",
        {
            dbName: dbName,
        },
    );

    const awaitShardClone = startParallelShell(() => {
        assert.commandWorked(
            db.adminCommand({
                _shardsvrCloneAuthoritativeMetadata: 1,
                databaseVersion: {
                    uuid: new UUID(),
                    timestamp: new Timestamp(1, 0),
                    lastMod: NumberInt(1),
                },
                writeConcern: {w: "majority"},
            }),
        );
    }, st.rs0.getPrimary().port);

    fp.wait();

    op();

    fp.off();
    awaitShardClone();

    // Validate that in shard0 we haven't cloned anything.
    st.awaitReplicationOnShards();
    validateShardCatalog(dbName, st.shard0, null /* expectedDbMetadata */);
    st.rs0.nodes.forEach((node) => {
        validateShardCatalogCache(dbName, node, null /* expectedDbMetadata */);
    });

    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
}

{
    jsTest.log("Validate concurrent movePrimary when running the cloning DDL");

    const dbName = "staleDb";
    const db = st.s.getDB(dbName);

    const movePrimary = () =>
        assert.commandWorked(db.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    validateCloningDDLWithConcurrentOperation(dbName, movePrimary);
}

{
    jsTest.log("Validate concurrent dropDatabase when running the cloning DDL");

    const dbName = "droppedDb";
    const db = st.s.getDB(dbName);

    const dropDatabase = () => assert.commandWorked(db.dropDatabase());

    validateCloningDDLWithConcurrentOperation(dbName, dropDatabase);
}

{
    jsTest.log(
        "Validate the cloning DDL is a no-op once reads are authoritative (fully-upgraded FCV)",
    );

    // Finish the upgrade so the shards become fully authoritative.
    assert.commandWorked(
        st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );

    const dbName = "noopDb";
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
    runCloningDDL(st.shard0);

    st.awaitReplicationOnShards();
    validateShardCatalog(dbName, st.shard0, getDbMetadataFromGlobalCatalog(st.s.getDB(dbName)));
}

st.stop();
