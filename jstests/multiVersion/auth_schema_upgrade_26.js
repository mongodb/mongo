/**
 * Tests a clean upgrade of user auth data from 2.4 to 2.6.
 *
 * Creates a 2.4 sharded cluster with auth enabled, and replicasets for the shards.
 * Upgrades the processes to 2.6.
 * Starts a 2.4 mongos, and confirms that upgrade cannot proceed; then stops that mongos.
 * Upgrades the auth schema on all the components.
 * Finally, verifies that all users are available where expected.
 */

load('jstests/multiVersion/libs/multi_cluster.js');
load('jstests/multiVersion/libs/multi_rs.js');

(function () {

    var oldVersion = '2.4';
    var newVersion = '2.6';
    var keyfile = 'jstests/libs/key1';

    /**
     * Logs out all connections "conn" from database "dbname".
     */
    function logout(conn, dbname) {
        var i;
        if (null == conn.length) {
            conn = [ conn ];
        }
        for (i = 0; i < conn.length; ++i) {
            conn[i].getDB(dbname).logout();
        }
    }

    /**
     * Authenticates all connections in "conns" using "authParams" on database "dbName".
     *
     * Raises an exception if any authentication fails, and tries to leave all connnections
     * in "conns" in the logged-out-of-dbName state.
     */
    function assertAuthenticate(conns, dbName, authParams) {
        var conn, i, ex, ex2;
        if (conns.length == null)
            conns = [ conns ];

        try {
            for (i = 0; i < conns.length; ++i) {
                conn = conns[i];
                assert(conn.getDB(dbName).auth(authParams),
                       "Failed to authenticate " + conn + " to " + dbName + " using parameters " +
                       tojson(authParams));
            }
        }
        catch (ex) {
            try {
                logout(conns, dbName);
            }
            catch (ex2) {
            }
            throw ex;
        }
    }

    /**
     * Authenticates all connections in "conns" using "authParams" on database "dbName".
     * Raises in exception if any of the authentications succeed.
     */
    function assertAuthenticateFails(conns, dbName, authParams) {
        var conn, i;
        if (conns.length == null)
            conns = [ conns ];

        for (i = 0; i < conns.length; ++i) {
            conn = conns[i];
            assert(!conn.getDB(dbName).auth(authParams),
                   "Unexpectedly authenticated " + conn + " to " + dbName + " using parameters " +
                   tojson(authParams));
        }
    }

    /**
     * Executes action() after authenticating the keyfile user on "conn", then logs out the keyfile
     * user.
     */
    function asCluster(conn, action) {
        var ex;
        assertAuthenticate(conn, 'local', {
            user: '__system',
            mechanism: 'MONGODB-CR',
            pwd: cat(keyfile).replace(/[ \n]/g, '')
        });

        try {
            action();
        }
        finally {
            try {
                logout(conn, 'local');
            }
            catch (ex) {
            }
        }
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

    // Update ReplSetTest.prototype.waitForIndicator to authenticate connections to the
    // replica set members using the keyfile, before attempting to perform operations.
    (function updateReplsetTestPrototypes() {
        var originalWaitForIndicator = ReplSetTest.prototype.waitForIndicator;
        ReplSetTest.prototype.waitForIndicator = function newRSTestWaitForIndicator(
            node, states, ind, timeout) {

            var self = this;
            if (node.length)
                return originalWaitForIndicator.apply(self, [node, states, ind, timeout]);
            asCluster(self.getMaster(), function () {
                originalWaitForIndicator.apply(self, [node, states, ind, timeout]);
            });
        };
    }());

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
