/*
 * Test for validating the correct behaviour of the CloneAuthoritativeMetadata DDL. This test aims
 * to check that the database metadata is cloned from the global catalog to the shard-local catalog
 * and cache correctly.
 *
 * NOTE: This test relies that there is no registration of authoritative database metadata in the
 * shard-local catalog initially, as it will test how the DDL works before implementing the full
 * upgrade procedure.
 *
 * @tags: [
 *     featureFlagShardAuthoritativeDbMetadataCRUD,
 *     featureFlagShardAuthoritativeDbMetadataDDL,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from 'jstests/libs/shardingtest.js';

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

function getDbMetadataFromGlobalCatalog(db) {
    return db.getSiblingDB('config').databases.findOne({_id: db.getName()});
}

function validateShardLocalCatalog(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.getDB('config').shard.databases.findOne({_id: dbName});
    assert.eq(expectedDbMetadata, dbMetadataFromShard);
}

function validateShardLocalCatalogCache(dbName, shard, expectedDbMetadata) {
    const dbMetadataFromShard = shard.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(dbMetadataFromShard);

    if (expectedDbMetadata) {
        assert.eq(expectedDbMetadata.version, dbMetadataFromShard.dbVersion);
    } else {
        assert.eq({}, dbMetadataFromShard.dbVersion);
    }
}

function runCloningDDL(conn) {
    assert.commandWorked(conn.adminCommand({
        _shardsvrCloneAuthoritativeMetadata: 1,
        databaseVersion: {
            uuid: new UUID(),
            timestamp: new Timestamp(1, 0),
            lastMod: NumberInt(1),
        },
        writeConcern: {w: "majority"}
    }));
}

{
    jsTest.log('Validate how the cloning DDL clones all databases from the global catalog');

    const fooDbName = 'foo';
    const barDbName = 'bar';

    const fooDb = st.s.getDB(fooDbName);
    const barDb = st.s.getDB(barDbName);

    assert.commandWorked(
        st.s.adminCommand({enableSharding: fooDbName, primaryShard: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({enableSharding: barDbName, primaryShard: st.shard1.shardName}));

    runCloningDDL(st.shard1);

    // Make sure the database metadata is installed on all nodes.
    st.awaitReplicationOnShards();

    // Validate that db metadata in the shard-local catalog and cache matches the global catalog.
    let dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(fooDb);
    validateShardLocalCatalog(fooDbName, st.shard1, dbMetadataFromConfig);
    st.rs1.nodes.forEach(node => {
        validateShardLocalCatalogCache(fooDbName, node, dbMetadataFromConfig);
    });

    dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(barDb);
    validateShardLocalCatalog(barDbName, st.shard1, dbMetadataFromConfig);
    st.rs1.nodes.forEach(node => {
        validateShardLocalCatalogCache(barDbName, node, dbMetadataFromConfig);
    });
}

{
    jsTest.log('Validate idempotency of the cloning DDL');

    const dbName = 'idempotentDb';

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    runCloningDDL(st.shard0);
    runCloningDDL(st.shard0);

    st.awaitReplicationOnShards();

    const dbMetadataFromConfig = getDbMetadataFromGlobalCatalog(st.s.getDB(dbName));
    validateShardLocalCatalog(dbName, st.shard0, dbMetadataFromConfig);
    st.rs0.nodes.forEach(node => {
        validateShardLocalCatalogCache(dbName, node, dbMetadataFromConfig);
    });
}

function validateCloningDDLWithConcurrentOperation(dbName, op) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    const fp = configureFailPoint(
        st.shard0, "hangAfterEnterInShardRoleCloneAuthoritativeMetadataDDL", {dbName: dbName});

    const awaitShardClone = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({
            _shardsvrCloneAuthoritativeMetadata: 1,
            databaseVersion: {
                uuid: new UUID(),
                timestamp: new Timestamp(1, 0),
                lastMod: NumberInt(1),
            },
            writeConcern: {w: "majority"}
        }));
    }, st.rs0.getPrimary().port);

    fp.wait();

    op();

    fp.off();
    awaitShardClone();

    // Validate that in shard0 we haven't cloned anything.
    st.awaitReplicationOnShards();
    validateShardLocalCatalog(dbName, st.shard0, null /* expectedDbMetadata */);
    st.rs0.nodes.forEach(node => {
        validateShardLocalCatalogCache(dbName, node, null /* expectedDbMetadata */);
    });
}

{
    jsTest.log('Validate concurrent movePrimary when running the cloning DDL');

    const dbName = "staleDb";
    const db = st.s.getDB(dbName);

    const movePrimary = () =>
        assert.commandWorked(db.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    validateCloningDDLWithConcurrentOperation(dbName, movePrimary);
}

{
    jsTest.log('Validate concurrent dropDatabase when running the cloning DDL');

    const dbName = "droppedDb";
    const db = st.s.getDB(dbName);

    const dropDatabase = () => assert.commandWorked(db.dropDatabase());

    validateCloningDDLWithConcurrentOperation(dbName, dropDatabase);
}

st.stop();
