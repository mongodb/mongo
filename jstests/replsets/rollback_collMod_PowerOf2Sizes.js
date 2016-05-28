// Test that a rollback of collModding usePowerOf2Sizes and validator can be rolled back.
//
// If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
// not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
// scenario, none of the members will have any data, and upon restart will each look for a member to
// inital sync from, so no primary will be elected. This test induces such a scenario, so cannot be
// run on ephemeral storage engines.
// @tags: [requires_persistence]
(function() {
    "use strict";

    function getOptions(conn) {
        return conn.getDB(name).foo.exists().options;
    }

    // Set up a set and grab things for later.
    var name = "rollback_collMod_PowerOf2Sizes";
    var replTest = new ReplSetTest({name: name, nodes: 3});
    var nodes = replTest.nodeList();
    var conns = replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    });
    // Get master and do an initial write.
    var master = replTest.getPrimary();
    var a_conn = master;
    var slaves = replTest.liveNodes.slaves;
    var b_conn = slaves[0];
    var AID = replTest.getNodeId(a_conn);
    var BID = replTest.getNodeId(b_conn);

    // Create collection with custom options.
    var originalCollectionOptions = {
        flags: 0,
        validator: {x: {$exists: 1}},
        validationLevel: "moderate",
        validationAction: "warn"
    };
    assert.commandWorked(a_conn.getDB(name).createCollection('foo', originalCollectionOptions));

    var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
    assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

    assert.eq(getOptions(a_conn), originalCollectionOptions);
    assert.eq(getOptions(b_conn), originalCollectionOptions);

    // Stop the slave so it never sees the collMod.
    replTest.stop(BID);

    // Run the collMod only on A.
    assert.commandWorked(a_conn.getDB(name).runCommand({
        collMod: "foo",
        usePowerOf2Sizes: false,
        noPadding: true,
        validator: {a: 1},
        validationLevel: "moderate",
        validationAction: "warn"
    }));
    assert.eq(getOptions(a_conn),
              {flags: 2, validator: {a: 1}, validationLevel: "moderate", validationAction: "warn"});

    // Shut down A and fail over to B.
    replTest.stop(AID);
    replTest.restart(BID);
    master = replTest.getPrimary();
    assert.eq(b_conn.host, master.host, "b_conn assumed to be master");
    b_conn = master;

    // Do a write on B so that A will have to roll back.
    options = {writeConcern: {w: 1, wtimeout: 60000}, upsert: true};
    assert.writeOK(b_conn.getDB(name).foo.insert({x: 2}, options));

    // Restart A, which should rollback the collMod before becoming primary.
    replTest.restart(AID);
    try {
        b_conn.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60});
    } catch (e) {
        // Ignore network disconnect.
    }
    replTest.waitForState(a_conn, ReplSetTest.State.PRIMARY);
    assert.eq(getOptions(a_conn), originalCollectionOptions);
}());
