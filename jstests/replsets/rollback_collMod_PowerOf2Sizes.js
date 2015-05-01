// Test that a rollback of collModding usePowerOf2Sizes and validator can be rolled back.
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
replTest.initiate({"_id": name,
                   "members": [
                       { "_id": 0, "host": nodes[0] },
                       { "_id": 1, "host": nodes[1] },
                       { "_id": 2, "host": nodes[2], arbiterOnly: true}]
                  });
// Get master and do an initial write.
var master = replTest.getMaster();
var a_conn = master;
var slaves = replTest.liveNodes.slaves;
var b_conn = slaves[0];
var AID = replTest.getNodeId(a_conn);
var BID = replTest.getNodeId(b_conn);

var options = {writeConcern: {w: 2, wtimeout: 60000}, upsert: true};
assert.writeOK(a_conn.getDB(name).foo.insert({x: 1}, options));

// Stop the slave so it never sees the collMod.
replTest.stop(BID);

// Run the collMod only on A.
assert.commandWorked(a_conn.getDB(name).runCommand({collMod: "foo",
                                                    usePowerOf2Sizes: false,
                                                    noPadding: true,
                                                    validator: {a: 1}}));
assert.eq(getOptions(a_conn), {flags: 2, validator: {a: 1}});

// Shut down A and fail over to B.
replTest.stop(AID);
replTest.restart(BID);
master = replTest.getMaster();
assert.eq(b_conn.host, master.host, "b_conn assumed to be master");
b_conn = master;

// Do a write on B so that A will have to roll back.
options = {writeConcern: {w: 1, wtimeout: 60000}, upsert: true};
assert.writeOK(b_conn.getDB(name).foo.insert({x: 2}, options));

// Restart A, which should rollback the collMod before becoming primary.
replTest.restart(AID);
try {
    b_conn.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 60});
}
catch (e) {
    // Ignore network disconnect.
}
replTest.waitForState(a_conn, replTest.PRIMARY);
assert.eq(getOptions(a_conn), {flags: 1}); // 1 is the default for flags.
}());
