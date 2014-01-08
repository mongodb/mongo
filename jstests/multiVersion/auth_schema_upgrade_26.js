/**
 * Tests a clean upgrade of user auth data from 2.4 to 2.6.
 *
 * Creates a 2.4 sharded cluster with auth enabled, and replicasets for the shards.
 * Upgrades the processes to 2.6.
 * Starts a 2.4 mongos, and confirms that upgrade cannot proceed; then stops that mongos.
 * Upgrades the auth schema on all the components.
 * Finally, verifies that all users are available where expected.
 */

load('jstests/multiVersion/libs/auth_support.js');
load('jstests/multiVersion/libs/multi_cluster.js');
load('jstests/multiVersion/libs/multi_rs.js');

(function () {

    var oldVersion = '2.4';
    var newVersion = '2.6';
    var keyfile = 'jstests/libs/key1';

    var logout = AuthSupport.logout;
    var assertAuthenticate = AuthSupport.assertAuthenticate;
    var assertAuthenticateFails = AuthSupport.assertAuthenticateFails;

    function asCluster(conn, action) {
        return AuthSupport.asCluster(conn, keyfile, action);
    }

    /**
     * Same as asCluster, but using the admin-cluster@admin user, instead of the keyfile user.
     */
    function asClusterAdmin(conn, action) {
        var ex;
        assertAuthenticate(conn, 'admin', { user: 'admin-cluster', pwd: 'a' });

        try {
            action();
        }
        finally {
            try {
                logout(conn, 'admin');
            }
            catch (ex) {
            }
        }
    }

    /**
     * Asserts that the correct administrative users appear in the correct nodes, as
     * demonstrated by the ability to authenticate with only the appropriate admin
     * user on each node.
     *
     * Particularly, admin-cluster should be authenticable on mongos and config servers,
     * admin-rs0 on members of the rs0 shard, and admin-rs1 on members of the rs1 shard.
     */
    function assertAdminUserConfiguration(shardingTest) {
        var i, node;

        asCluster(shardingTest.rs0.nodes, function () { shardingTest.rs0.awaitReplication(); });
        asCluster(shardingTest.rs1.nodes, function () { shardingTest.rs1.awaitReplication(); });

        assert.gt(shardingTest.rs0.nodes.length, 0);
        for (i = 0; i < shardingTest.rs0.nodes.length; ++i) {
            node = shardingTest.rs0.nodes[i];
            assertAuthenticate(
                new Mongo(node.host), 'admin', { user: 'admin-rs0', pwd: 'a' });
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-rs1', pwd: 'a' });
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-cluster', pwd: 'a' });
        }

        assert.gt(shardingTest.rs1.nodes.length, 0);
        for (i = 0; i < shardingTest.rs1.nodes.length; ++i) {
            node = shardingTest.rs1.nodes[i];
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-rs0', pwd: 'a' });
            assertAuthenticate(
                new Mongo(node.host), 'admin', { user: 'admin-rs1', pwd: 'a' });
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-cluster', pwd: 'a' });
        }

        assert.gt(shardingTest._mongos.length, 0);
        for (i = 0; i < shardingTest._mongos.length; ++i) {
            node = shardingTest._mongos[i];
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-rs0', pwd: 'a' });
            assertAuthenticateFails(
                new Mongo(node.host), 'admin', { user: 'admin-rs1', pwd: 'a' });
            assertAuthenticate(
                new Mongo(node.host), 'admin', { user: 'admin-cluster', pwd: 'a' });
        }
    }

    /**
     * Adds several 2.4 style users to the cluster and individual shards, used during
     * the test.
     */
    function setUpV24Users(shardingTest) {
        print('\n--------------------\n' +
              'Adding 2.4-style root users to admin database of cluster and each shard\n' +
              '\n--------------------\n');

        shardingTest.s0.getDB('admin').addUser('admin-cluster', 'a');
        shardingTest.rs0.getPrimary().getDB('admin').addUser('admin-rs0', 'a');
        shardingTest.rs1.getPrimary().getDB('admin').addUser('admin-rs1', 'a');
        assertAdminUserConfiguration(shardingTest);

        var s0AdminConn = new Mongo(shardingTest.s0.host);
        assertAuthenticate(s0AdminConn, 'admin', { user: 'admin-cluster', pwd: 'a' });

        print('\n--------------------\n' +
              'Adding 2.4-style users to t1 and t2 databases of cluster\n' +
              '\n--------------------\n');
        s0AdminConn.getDB('t1').addUser({ user: 't1-user', pwd: 'a', roles: [ 'readWrite' ]});
        s0AdminConn.getDB('t2').addUser({ user: 't2-user', pwd: 'a', roles: [ 'read' ]});

        assertAuthenticate(s0AdminConn, 't1', { user: 't1-user', pwd: 'a' });
        assertAuthenticate(s0AdminConn, 't2', { user: 't2-user', pwd: 'a' });
    }

    var initialOptions = {
        name: 'auth_schema_upgrade_normal',
        rs: { nodes: 3 },
        mongosOptions: { binVersion: oldVersion },
        configOptions: { binVersion: oldVersion },
        shardOptions: { binVersion: oldVersion },
    };

    var shardingTest = new ShardingTest({
        shards: 2,
        mongos: 2,
        other: initialOptions,
        keyFile: keyfile
    });

    setUpV24Users(shardingTest);

    print('\n--------------------\n' +
          'Upgrading all process binaries to 2.6.' +
          '\n--------------------\n');
    asClusterAdmin(shardingTest.config.getMongo(), function stopBalancerAsAdmin() {
        shardingTest.stopBalancer();
    });

    var mongos = MongoRunner.runMongos({
        binVersion : newVersion,
        configdb : shardingTest._configDB,
        upgrade : "",
        keyFile: keyfile
    });
    assert.neq(null, mongos);
    MongoRunner.stopMongos(mongos);
    shardingTest.upgradeCluster(newVersion, { upgradeMetadata: true });
    asClusterAdmin(shardingTest.config.getMongo(), function restartBalancerAsAdmin() {
        shardingTest.startBalancer();
    });

    assertAdminUserConfiguration(shardingTest);

    var s0AdminConn = new Mongo(shardingTest.s0.host);
    assertAuthenticate(s0AdminConn, 'admin', { user: 'admin-cluster', pwd: 'a' });

    print('\n--------------------\n' +
          'Attempting upgrade with old mongos still running; should fail.' +
          '\n--------------------\n');
    var oldMongoS = MongoRunner.runMongos({
        binVersion : oldVersion,
        configdb : shardingTest._configDB,
        keyFile: keyfile
    });
    assert.neq(null, oldMongoS);
    assert.commandFailedWithCode(
        s0AdminConn.getDB('admin').runCommand({authSchemaUpgrade: 1}),
        25,
        "Expected RemoteValidationFailed error code");
    MongoRunner.stopMongos(oldMongoS);

    s0AdminConn.getDB("config").mongos.remove();
    assert.gleSuccess(s0AdminConn.getDB("config"), "Flushing ping time data failed.");

    print('\n--------------------\n' +
          'Upgrading auth schema.' +
          '\n--------------------\n');
    assert.commandWorked(s0AdminConn.getDB('admin').runCommand({authSchemaUpgrade: 1}));

    print('\n--------------------\n' +
          'Validating upgraded user configuration.' +
          '\n--------------------\n');
    assertAdminUserConfiguration(shardingTest);
    assertAuthenticate(s0AdminConn, 't1', { user: 't1-user', pwd: 'a' });
    assertAuthenticate(s0AdminConn, 't2', { user: 't2-user', pwd: 'a' });
    logout(s0AdminConn, 't1');
    logout(s0AdminConn, 't2');
}());
