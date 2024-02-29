/**
 * Test that servers with incorrect cluster roles (for the replica set they are in) eventually crash
 * during the upgrade process from last-lts to latest (and continue to crash if restarted with an
 * incorrect cluster role). Also test that there are no issues with CRUD operations or data
 * inconsistencies from a replica set with mixed cluster roles.
 *
 * To provide context on this test: SERVER-80249 added code to allow a last-lts replica set
 * to restart as an latest auto-bootstrapped config shard; however, that code also necessarily
 * allows a replica set to be started with mixed cluster roles. To fix that, SERVER-80249 also added
 * code to shut down a server if its cluster role does not align with the shard identity document it
 * replicated (example: shard server has shard identity document for a config server). This ensures
 * that the replica set eventually all has nodes with the same cluster role.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   requires_persistence
 * ]
 */

function testCRUD(conn) {
    const db = conn.getDB('apple');
    assert.commandWorked(db.foo.insert({x: 1}));
    assert.commandWorked(db.foo.insert({x: -1}));
    assert.commandWorked(db.foo.update({x: 1}, {$set: {y: 1}}));
    assert.commandWorked(db.foo.update({x: -1}, {$set: {y: 1}}));
    var doc1 = db.foo.findOne({x: 1});
    assert.eq(1, doc1.y);
    var doc2 = db.foo.findOne({x: -1});
    assert.eq(1, doc2.y);

    assert.commandWorked(db.foo.remove({x: 1}, true));
    assert.commandWorked(db.foo.remove({x: -1}, true));
    assert.eq(null, db.foo.findOne());

    return true;
}

function ensureShardingCommandsFail(conn) {
    assert.commandFailedWithCode(
        conn.getDB('admin').runCommand({_shardsvrCreateCollection: 'test.test', key: {_id: 1}}),
        [ErrorCodes.ShardingStateNotInitialized, ErrorCodes.NoShardingEnabled_OBSOLETE]);
}

/**
 * Upgrading last-lts replica set to latest one shard cluster (config shard).
 *
 * The test first verifies that CRUD operations behave correctly before the shard identity document
 * is inserted. It does this verification with various primaries (shard server primary, or last-lts
 * replica set node primary) and secondaries (shard server secondary, config server secondary
 * and last-lts replica set node secondary).
 *
 * The test then verifies that shard servers crash when the config server shard identity
 * document is inserted.
 *
 * Finally it verifies that there were no inconsistences as a result of the mixed cluster roles
 * before the shard identity document was inserted.
 */
{
    jsTestLog('Starting a replica set with 4 nodes (only the first node can be elected).');
    let rst = new ReplSetTest({
        name: 'rs',
        nodes: [
            // Only allow elections on node1 to make it easier to control what ClusterRole the
            // primary has.
            {},
            {rsConfig: {priority: 0}},
            {rsConfig: {priority: 0}},
            {rsConfig: {priority: 0}}
        ],
        nodeOptions: {binVersion: 'last-lts'}
    });
    rst.startSet();
    rst.initiate();
    rst.getPrimary();
    rst.awaitSecondaryNodes();
    let [node0, node1, node2, node3] = rst.nodes;

    jsTestLog('Restarting node1 as shard server and node2 as config server.');
    MongoRunner.stopMongod(node1, null, {noCleanData: true});
    MongoRunner.stopMongod(node2, null, {noCleanData: true});
    node1 = MongoRunner.runMongod({
        noCleanData: true,
        shardsvr: '',
        replSet: 'rs',
        dbpath: node1.dbpath,
        port: node1.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });
    node2 = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: 'rs',
        dbpath: node2.dbpath,
        port: node2.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });

    jsTestLog('Test with last-lts node as primary before the shard identity document is inserted.');
    assert.eq(rst.getPrimary(), node0);
    testCRUD(node0);
    ensureShardingCommandsFail(node0);

    jsTestLog('Test with shard server primary before the shard identity document is inserted.')
    MongoRunner.stopMongod(node0, null, {noCleanData: true});
    node0 = MongoRunner.runMongod({
        noCleanData: true,
        shardsvr: '',
        replSet: 'rs',
        dbpath: node0.dbpath,
        port: node0.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });
    assert.eq(rst.getPrimary(), node0);
    testCRUD(node0);
    ensureShardingCommandsFail(node0);

    jsTestLog('Restarting with config server as primary.');
    MongoRunner.stopMongod(node0, null, {noCleanData: true});
    node0 = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: 'rs',
        dbpath: node0.dbpath,
        port: node0.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });

    jsTestLog('Upgrading the last-lts replica set node to a latest config server.');
    MongoRunner.stopMongod(node3, null, {noCleanData: true});
    node3 = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: 'rs',
        dbpath: node3.dbpath,
        port: node3.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });

    jsTestLog(
        'Wait for config server primary to be writable primary (which implies shard identity document is inserted).');
    assert.soonNoExcept(() => testCRUD(node0));

    jsTestLog('Checking that the node1 shard server crashes.');
    assert.soon(() => !checkProgram(node1.pid).alive);
    [node0, node2, node3].map(node => checkProgram(node.pid).alive);

    jsTestLog('Restarting the crashed node1 server as a config server should work.');
    node1 = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: 'rs',
        dbpath: node1.dbpath,
        port: node1.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });
    assert.commandWorked(node1.getDB('admin').runCommand({ping: 1}));

    jsTestLog('Restarting a config server as a shard server should fail.');
    rst.awaitSecondaryNodes();  // Ensure shard identity document replicated to secondaries
    MongoRunner.stopMongod(node2, null, {noCleanData: true});
    assert.throws(() => {
        MongoRunner.runMongod({
            noCleanData: true,
            shardsvr: '',
            replSet: 'rs',
            dbpath: node2.dbpath,
            port: node2.port,
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        });
    });
    node2 = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: 'rs',
        dbpath: node2.dbpath,
        port: node2.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });

    // stopSet() will also check oplogs, preImageCollection, changeCollection and db hashes
    // which ensures that the mixed cluster roles for the replica set at the start did not cause
    // any data inconsistencies.
    //
    // Collection validation is skipped because unclean shutdowns (due to the shard identity
    // document not matching the shard's cluster role) causes fast counts to be wrong for
    // admin.system.version.
    rst.stopSet(null, null, {skipValidation: true});
}

/**
 * Upgrading last-lts sharded cluster to latest sharded cluster.
 *
 * This test verifies that mixed cluster roles are not possible when upgrading a last-lts sharded
 * cluster to latest. More specifically it verifies a server will crash on startup in latest if its
 * cluster role does not agree with the shard identity for the replica set. For example if a shard
 * server sees the shard identity document for a config server (i.e with _id: 'config') it should
 * crash because it knows the replica set it is a part of is for a config server.
 */
{
    const st = new ShardingTest({
        mongos: 1,
        config: 3,
        shards: 1,
        rs: {nodes: 3},
        configShard: false,
        nodeOptions: {binVersion: 'last-lts'}
    });

    jsTestLog(
        'Restarting a last-lts config server secondary as a latest shard server should fail.');
    let configSecondary = st.configRS.getSecondary();
    printjson(configSecondary);
    MongoRunner.stopMongod(configSecondary, null, {noCleanData: true});
    assert.throws(() => {
        MongoRunner.runMongod({
            noCleanData: true,
            shardsvr: '',
            replSet: st.configRS.name,
            dbpath: configSecondary.dbpath,
            port: configSecondary.port,
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        });
    });

    jsTestLog(
        'Restarting a last-lts config server secondary as a latest config server should work.');
    configSecondary = MongoRunner.runMongod({
        noCleanData: true,
        configsvr: '',
        replSet: st.configRS.name,
        dbpath: configSecondary.dbpath,
        port: configSecondary.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });
    testCRUD(st.s0);

    jsTestLog(
        'Restarting a last-lts shard server secondary as a latest config server should fail.');
    let shardSecondary = st.rs0.getSecondary();
    MongoRunner.stopMongod(shardSecondary, null, {noCleanData: true});
    assert.throws(() => {
        shardSecondary = MongoRunner.runMongod({
            noCleanData: true,
            configsvr: '',
            replSet: st.rs0.name,
            dbpath: shardSecondary.dbpath,
            port: shardSecondary.port,
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        });
    });

    jsTestLog('Restarting a last-lts shard server secondary as a latest shard server should work.');
    shardSecondary = MongoRunner.runMongod({
        noCleanData: true,
        shardsvr: '',
        replSet: st.rs0.name,
        dbpath: shardSecondary.dbpath,
        port: shardSecondary.port,
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
        }
    });
    testCRUD(st.s0);

    st.stop();
}
