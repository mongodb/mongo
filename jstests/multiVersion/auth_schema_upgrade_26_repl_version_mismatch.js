/**
 * Tests that replica set upgrade won't proceed if a member of the cluster that is up is running
 * a binary version that is too old, but will proceed if that member is down.
 */

load('jstests/multiVersion/libs/multi_rs.js');

(function () {

    var oldVersion = '2.4';
    var newVersion = '2.6';
    var keyfile = 'jstests/libs/key1';

    var logout = authutil.logout;
    var assertAuthenticate = authutil.assertAuthenticate;
    var assertAuthenticateFails = authutil.assertAuthenticateFails;

    function asCluster(conn, action) {
        return authutil.asCluster(conn, keyfile, action);
    }

    var rst = new ReplSetTest({
        nodes: [
            { binVersion: newVersion },
            { binVersion: newVersion },
            { binVersion: oldVersion }
        ],
        nodeOptions: { keyFile: keyfile }
    });

    var cfg = rst.getReplSetConfig();
    cfg.members[2].priority = 0;
    rst.startSet();
    rst.initiate();
    rst.awaitReplication();

    var primary = rst.getPrimary();
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({authSchemaUpgrade: 1}),
        25,
        "Mixed version cluster should not have allowed auth schema upgrade.");
    rst.stop(2, undefined, true);
    assert.commandWorked(primary.getDB("admin").runCommand({authSchemaUpgrade: 1}));
}());
