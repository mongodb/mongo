// Tests that the creation of a user replicates to secondaries, and survives a restart of all
// data-bearing nodes.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]

(function() {
    "use strict";
    var keyfile = "jstests/libs/key1";
    var master;
    var rs = new ReplSetTest({nodes: {node0: {}, node1: {}, arbiter: {}}, keyFile: keyfile});
    rs.startSet();
    rs.initiate();

    master = rs.getPrimary();
    jsTest.log("adding user");
    master.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles},
                                     {w: 2, wtimeout: 30000});

    var safeInsert = function() {
        master = rs.getPrimary();
        master.getDB("admin").auth("foo", "bar");
        assert.writeOK(master.getDB("foo").bar.insert({x: 1}));
    };

    jsTest.log("authing");
    for (var i = 0; i < 2; i++) {
        assert(rs.nodes[i].getDB("admin").auth("foo", "bar"),
               "could not log into " + rs.nodes[i].host);
    }

    jsTest.log("make common point");

    safeInsert();
    authutil.asCluster(rs.nodes, keyfile, function() {
        rs.awaitReplication();
    });

    jsTest.log("write stuff to 0&2");
    rs.stop(1);

    master = rs.getPrimary();
    master.getDB("admin").auth("foo", "bar");
    master.getDB("foo").bar.drop();
    jsTest.log("last op: " +
               tojson(master.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next()));

    jsTest.log("write stuff to 1&2");
    rs.stop(0);
    rs.restart(1);

    safeInsert();
    jsTest.log("last op: " +
               tojson(master.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next()));

    rs.restart(0);

    jsTest.log("doing rollback!");

    authutil.asCluster(rs.nodes, keyfile, function() {
        rs.awaitSecondaryNodes();
    });

}());
