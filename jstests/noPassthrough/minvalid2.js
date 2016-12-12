/**
 * This checks rollback, which shouldn't happen unless we have reached minvalid.
 *  1. make 3-member set w/arb (2)
 *  2. shut down slave
 *  3. do writes to master
 *  4. modify master's minvalid
 *  5. shut down master
 *  6. start up slave
 *  7. writes on former slave (now primary)
 *  8. start up master
 *  9. check master does not rollback
 *
 * If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
 * not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
 * scenario, none of the members will have any data, and upon restart will each look for a member to
 * initial sync from, so no primary will be elected. This test induces such a scenario, so cannot be
 * run on ephemeral storage engines.
 * @tags: [requires_persistence]
 */

print("1. make 3-member set w/arb (2)");
var name = "minvalid";
var replTest = new ReplSetTest({name: name, nodes: 3, oplogSize: 1});
var host = getHostName();

var nodes = replTest.startSet();
replTest.initiate({
    _id: name,
    members: [
        {_id: 0, host: host + ":" + replTest.ports[0]},
        {_id: 1, host: host + ":" + replTest.ports[1]},
        {_id: 2, host: host + ":" + replTest.ports[2], arbiterOnly: true}
    ]
});
var slaves = replTest.liveNodes.slaves;
var master = replTest.getPrimary();
var masterId = replTest.getNodeId(master);
var slave = slaves[0];
var slaveId = replTest.getNodeId(slave);

// Wait for primary to detect that the arbiter is up so that it won't step down when we later take
// the secondary offline.
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.ARBITER);

var mdb = master.getDB("foo");

mdb.foo.save({a: 1000});
replTest.awaitReplication();

print("2: shut down slave");
replTest.stop(slaveId);

print("3: write to master");
assert.writeOK(mdb.foo.insert({a: 1001}, {writeConcern: {w: 1}}));

print("4: modify master's minvalid");
var local = master.getDB("local");
var lastOp = local.oplog.rs.find().sort({$natural: -1}).limit(1).next();
printjson(lastOp);

// Overwrite minvalid document to simulate an inconsistent state (as might result from a server
// crash.
local.replset.minvalid.update(
    {}, {ts: new Timestamp(lastOp.ts.t, lastOp.ts.i + 1)}, {upsert: true});
printjson(local.replset.minvalid.findOne());

print("5: shut down master");
replTest.stop(masterId);

print("6: start up slave");
replTest.restart(slaveId);

print("7: writes on former slave");
master = replTest.getPrimary();
mdb1 = master.getDB("foo");
mdb1.foo.save({a: 1002});

print("8: start up former master");
clearRawMongoProgramOutput();
replTest.restart(masterId);

print("9: check former master " + replTest.nodes[masterId].host + " does not select former slave " +
      slave.host + " as sync source");
replTest.waitForState(replTest.nodes[masterId], ReplSetTest.State.RECOVERING, 90000);

// Sync source selection will log this message if it does not detect min valid in the sync
// source candidate's oplog.
assert.soon(function() {
    return rawMongoProgramOutput().match(
        'it does not contain the necessary operations for us to reach a consistent state');
});

replTest.stopSet(undefined, undefined, {allowedExitCodes: [MongoRunner.EXIT_ABRUPT]});
