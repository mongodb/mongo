/**
 * This checks rollback, which shouldn't happen unless we have reached minvalid.
 *  1. make 3-member set w/arb (2)
 *  2. shut down secondary
 *  3. do writes to primary
 *  4. modify primary's minvalid
 *  5. shut down primary
 *  6. start up secondary
 *  7. writes on former secondary (now primary)
 *  8. start up primary
 *  9. check primary does not rollback
 *
 * If all data-bearing nodes in a replica set are using an ephemeral storage engine, the set will
 * not be able to survive a scenario where all data-bearing nodes are down simultaneously. In such a
 * scenario, none of the members will have any data, and upon restart will each look for a member to
 * initial sync from, so no primary will be elected. This test induces such a scenario, so cannot be
 * run on ephemeral storage engines.
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   sbe_incompatible,
 * ]
 */

// Skip db hash check because replset cannot reach consistent state.
TestData.skipCheckDBHashes = true;

print("1. make 3-member set w/arb (2)");
var name = "minvalid";
var replTest = new ReplSetTest({name: name, nodes: 3, oplogSize: 1, waitForKeys: true});
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
var secondaries = replTest.getSecondaries();
var primary = replTest.getPrimary();
var primaryId = replTest.getNodeId(primary);
var secondary = secondaries[0];
var secondaryId = replTest.getNodeId(secondary);

// Wait for primary to detect that the arbiter is up so that it won't step down when we later take
// the secondary offline.
replTest.waitForState(replTest.nodes[2], ReplSetTest.State.ARBITER);

var mdb = primary.getDB("foo");

mdb.foo.save({a: 1000});
replTest.awaitReplication();

print("2: shut down secondary");
replTest.stop(secondaryId);

print("3: write to primary");
assert.commandWorked(mdb.foo.insert({a: 1001}, {writeConcern: {w: 1}}));

print("4: modify primary's minvalid");
var local = primary.getDB("local");
var lastOp = local.oplog.rs.find().sort({$natural: -1}).limit(1).next();
printjson(lastOp);

// Overwrite minvalid document to simulate an inconsistent state (as might result from a server
// crash.
local.replset.minvalid.update({},
                              {
                                  ts: new Timestamp(lastOp.ts.t, lastOp.ts.i + 1),
                                  t: NumberLong(-1),
                              },
                              {upsert: true});
printjson(local.replset.minvalid.findOne());

print("5: shut down primary");
replTest.stop(primaryId);

print("6: start up secondary");
replTest.restart(secondaryId);

print("7: writes on former secondary");
primary = replTest.getPrimary();
mdb1 = primary.getDB("foo");
mdb1.foo.save({a: 1002});

print("8: start up former primary");
clearRawMongoProgramOutput();
replTest.restart(primaryId);

print("9: check former primary " + replTest.nodes[primaryId].host +
      " does not select former secondary " + secondary.host + " as sync source");
replTest.waitForState(replTest.nodes[primaryId], ReplSetTest.State.RECOVERING, 90000);

// Sync source selection will log this message if it does not detect min valid in the sync
// source candidate's oplog.
assert.soon(function() {
    return rawMongoProgramOutput().match(
        'it does not contain the necessary operations for us to reach a consistent state');
});

replTest.stopSet();
